#include "gst_streamer.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>

namespace camstream {

GstStreamer::GstStreamer() : pipeline_(nullptr), appsrc_(nullptr), bus_(nullptr) {}

GstStreamer::~GstStreamer() {
    destroy();
}

void GstStreamer::destroy() {
    // 镜像原 main cleanup 的释放顺序; 幂等, 可在析构前显式调用
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        if (bus_) { gst_object_unref(bus_); bus_ = nullptr; }
        if (appsrc_) { gst_object_unref(appsrc_); appsrc_ = nullptr; }
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}

// ====== 内嵌 GStreamer 硬件编码推流管线 ======
// VC8000E 走 V4L2 无状态编码 uAPI, v4l2slh264enc 是其配套用户态实现
// (含用户态 SPS/PPS 生成与码控), 自研客户端代价过高, 这里在程序内复用.
// 用 appsrc 注入完整一帧(带 GstVideoMeta)的缓冲.
bool GstStreamer::build(const char *ip, int port, uint32_t width,
                        uint32_t height, int bitrate_kbps, int fps,
                        const char *sdp_file, int gop, int colorimetry,
                        int max_queue_frames) {
    GError *gerr = nullptr;
    GstCaps *caps = nullptr;
    std::ostringstream desc, caps_str;

    gst_init(nullptr, nullptr);

    // GOP: 显式 --gop N 优先; 默认保持原公式(约 1 IDR/秒)
    const int keyframe_interval = (gop > 0) ? gop : (fps < 10 ? 10 : fps);
    // colorimetry 显式声明, 消除编码器/解码端猜测(plan §4.4)
    const char *col = (colorimetry == 1) ? "bt709" : "bt601";

    desc << "appsrc name=frame_src"
         << " ! videoconvert"
         << " ! video/x-raw,format=NV12,width=" << width << ",height=" << height
         << ",framerate=" << fps << "/1,colorimetry=" << col
         << " ! v4l2slh264enc bitrate=" << (bitrate_kbps * 1000)
         << " keyframe-interval=" << keyframe_interval
         << " ! h264parse config-interval=1"
         << " ! rtph264pay mtu=1400 config-interval=1 pt=96 aggregate-mode=zero-latency"
         << " ! udpsink host=" << ip << " port=" << port << " sync=false async=false";
    std::cout << "Pipeline: " << desc.str() << std::endl;

    pipeline_ = gst_parse_launch(desc.str().c_str(), &gerr);
    if (!pipeline_) {
        std::cerr << "gst_parse_launch failed: "
                  << (gerr ? gerr->message : "unknown") << std::endl;
        if (gerr) g_error_free(gerr);
        return false;
    }
    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "frame_src");
    if (!appsrc_) {
        std::cerr << "appsrc 'frame_src' not found in pipeline" << std::endl;
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return false;
    }

    caps_str << "video/x-raw,format=NV12,width=" << width << ",height=" << height
             << ",framerate=" << fps << "/1,colorimetry=" << col;
    caps = gst_caps_from_string(caps_str.str().c_str());
    gst_app_src_set_caps(GST_APP_SRC(appsrc_), caps);
    gst_caps_unref(caps);
    if (max_queue_frames < 1)
        max_queue_frames = 1;
    g_object_set(appsrc_,
                 "is-live", (gboolean)TRUE,
                 "block", (gboolean)TRUE,
                 "do-timestamp", (gboolean)TRUE,
                 "format", (gint)GST_FORMAT_TIME,
                 "max-bytes", (guint64)((size_t)width * height * 3 / 2 *
                                        (size_t)max_queue_frames),
                 nullptr);

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Failed to set pipeline to PLAYING" << std::endl;
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return false;
    }

    // 生成接收端 SDP 文件(连接地址为接收主机, 单播场景即 udpsink 目标)
    {
        std::ofstream sdp(sdp_file);
        sdp << "v=0\n"
            << "o=- 0 0 IN IP4 " << ip << "\n"
            << "s=rgb_to_nv12\n"
            << "c=IN IP4 " << ip << "\n"
            << "t=0 0\n"
            << "m=video " << port << " RTP/AVP 96\n"
            << "a=rtpmap:96 H264/90000\n";
    }
    std::cout << "Receiver(PC): ffplay -protocol_whitelist file,udp,rtp "
                 "-fflags nobuffer -flags low_delay -framedrop "
                 "-probesize 32768 -analyzeduration 0 " << sdp_file << "\n"
              << "             vlc --network-caching=100 " << sdp_file
              << "  (rtp:// URI 不支持动态载荷96, 须用 sdp 文件)"
              << std::endl;

    bus_ = gst_element_get_bus(pipeline_);   // 调用方(本类)持有此引用
    return true;
}

// 构造"精确一帧 + GstVideoMeta"的缓冲推入 appsrc, 返回 0 成功
int GstStreamer::pushFrame(const uint8_t *data, size_t size,
                           uint32_t width, uint32_t height) {
    GstBuffer *buf = gst_buffer_new_allocate(nullptr, size, nullptr);
    if (!buf) return -1;
    gst_buffer_fill(buf, 0, data, size);
    gst_buffer_add_video_meta(buf, (GstVideoFrameFlags)0,
                              GST_VIDEO_FORMAT_NV12, (guint)width, (guint)height);
    return (gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf) == GST_FLOW_OK)
               ? 0 : -1;
}

uint64_t GstStreamer::queuedBytes() const {
    if (!appsrc_)
        return 0;
    guint64 level = 0;
    g_object_get(appsrc_, "current-level-bytes", &level, nullptr);
    return (uint64_t)level;
}

// 非阻塞轮询管线总线, 返回 0 正常, -1 管线出错(无需 GLib 主循环)
int GstStreamer::checkBus() {
    GstMessage *msg;
    while ((msg = gst_bus_pop(bus_)) != nullptr) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *e = nullptr;
            gchar *dbg = nullptr;
            gst_message_parse_error(msg, &e, &dbg);
            std::cerr << "Pipeline error: " << (e ? e->message : "unknown") << std::endl;
            if (dbg) { std::cerr << "  debug: " << dbg << std::endl; g_free(dbg); }
            if (e) g_error_free(e);
            gst_message_unref(msg);
            return -1;
        }
        gst_message_unref(msg);
    }
    return 0;
}

} // namespace camstream
