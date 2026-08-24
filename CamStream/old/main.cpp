/*
 * RGB24(摄像头) --OpenCL(GPU)--> NV12 --appsrc--> GStreamer 硬件H264编码 --UDP--> RTP 推流
 *
 * 数据通路:
 *   启动时自动用 media-ctl 重建 DCMIPP 链路并设置各级格式与传感器
 *   增益/曝光(重启后内核复位为 640x480, 不重配则 STREAMON 失败)
 *   /dev/video1 (DCMIPP, RGB888_1X24 1920x1080)
 *     -> poll + DQBUF (mmap 缓冲区只在初始化时映射一次, 热路径零映射开销)
 *     -> clEnqueueWriteBuffer 显式写入 GPU 设备内存
 *        (USE_HOST_PTR 直读在嵌入式 GPU 上有一致性风险, 见 build 处注释)
 *     -> OpenCL 内核 RGB888->NV12 (每个 work-item 处理 2x2 像素块)
 *     -> clEnqueueReadBuffer 非阻塞 + event, 双缓冲流水线
 *        (GPU 转换第 N 帧的同时, CPU 采集第 N+1 帧)
 *     -> appsrc 注入完整一帧(带 GstVideoMeta)到程序内构建的 GStreamer 管线:
 *        appsrc ! videoconvert(直通隔离) ! v4l2slh264enc(VC8000E 硬件编码)
 *          ! h264parse ! rtph264pay ! udpsink
 *        (RTP mtu=1400, 每个 UDP 报文 <= MTU, 规避单报文超过 IP 数据报 64KB 上限.
 *         不能用 fdsrc 经管道喂数据: 管道容量 64KB 会产生短缓冲,
 *         v4l2slh264enc 收到声称 1080p 实则 64KB 的缓冲会直接崩溃)
 *     程序启动时在当前目录生成 stream.sdp, PC 端 ffplay/VLC 按其播放
 *
 * 用法: ./rgb_to_nv12 [采集设备] [目标IP] [端口] [码率kbps]
 * 默认: /dev/video1 192.168.137.1 5000 4000
 * Ctrl-C 退出并清理资源.
 */

#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

// OpenCL headers
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 300
#endif
#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

// GStreamer: appsrc 注入 + 硬件编码推流
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

// ====== 默认配置 ======
#define CAMERA_DEVICE   "/dev/video1"     // DCMIPP main postproc 输出节点 (RGB888_1X24)
#define VIDEO_WIDTH     1920
#define VIDEO_HEIGHT    1080              // 内核按 2x2 块处理, 要求宽高均为偶数
#define DEST_IP         "192.168.137.1"   // 接收主机IP
#define DEST_PORT       5000
#define BITRATE_KBPS    4000              // H264 目标码率
#define CAP_BUF_COUNT   4                 // V4L2 采集缓冲区个数
#define PIPELINE_FPS    30                // 告知编码管线的帧率

// ====== 错误检查宏 ======
#define CHECK_CL_ERROR(err, msg) \
    if ((err) != CL_SUCCESS) { \
        std::cerr << "OpenCL Error [" << msg << "]: " << err << std::endl; \
        goto cleanup; \
    }

// ====== 全局运行开关 ======
static volatile sig_atomic_t g_stop = 0;
static void handle_signal(int) { g_stop = 1; }

// ====== DCMIPP media 管线配置 ======
// 重启后 DCMIPP 链路/格式/传感器参数全部复位(内核默认 640x480), 直接
// STREAMON 会报 "Wrong width or height ... (640x480 expected)".
// 这里执行与板端验证过的 py 脚本相同的 media-ctl 序列:
//   重建链路 csi->input->isp->postproc->capture, 逐 pad 设格式
//   (传感器->CSI->input 为 SRGGB10, ISP 输出起为 RGB888_1X24),
//   并手动设置传感器增益/曝光(本路径没有 libcamera 3A, 不设会黑屏;
//   subdev 节点通过解析 media-ctl -p 动态获取, 防跨重启编号漂移).
static int run_cmd(const std::string &cmd) {
    int ret = system(cmd.c_str());
    if (ret != 0)
        std::cerr << "Command failed (" << ret << "): " << cmd << std::endl;
    return ret;
}

// 从 media-ctl -p 输出解析实体对应的 /dev/v4l-subdevN
// (subdev 编号跨重启会漂移, 不能硬编码)
static std::string find_subdev(const char *entity_hint) {
    FILE *p = popen("media-ctl -d platform:48030000.dcmipp -p 2>/dev/null", "r");
    std::string result;
    if (!p) return result;
    char line[512];
    bool seen_entity = false;
    while (fgets(line, sizeof(line), p)) {
        if (!seen_entity) {
            if (strstr(line, entity_hint)) seen_entity = true;
            continue;
        }
        const char *dn = strstr(line, "device node name ");
        if (dn) {
            result = dn + strlen("device node name ");
            while (!result.empty() &&
                   (result.back() == '\n' || result.back() == '\r'))
                result.pop_back();
            break;
        }
    }
    pclose(p);
    return result;
}

static bool setup_dcmipp_pipeline(uint32_t width, uint32_t height) {
    const std::string md = "media-ctl -d platform:48030000.dcmipp ";
    std::ostringstream raw_fmt, rgb_fmt, cc;

    raw_fmt << "SRGGB10_1X10/" << width << "x" << height;
    rgb_fmt << "RGB888_1X24/" << width << "x" << height;
    cc << " crop:(0,0)/" << width << "x" << height
       << " compose:(0,0)/" << width << "x" << height;

    // 清理可能占用设备的残留 gst 进程(与 py 脚本做法一致)
    (void)system("killall -9 gst-launch-1.0 2>/dev/null");

    if (run_cmd(md + "-r") != 0) return false;
    if (run_cmd(md + "-l '\"48020000.csi\":1->\"dcmipp_input\":0[1]'") != 0) return false;
    if (run_cmd(md + "-l '\"dcmipp_input\":2->\"dcmipp_main_isp\":0[1]'") != 0) return false;
    if (run_cmd(md + "-l '\"dcmipp_main_isp\":1->\"dcmipp_main_postproc\":0[1]'") != 0) return false;
    if (run_cmd(md + "-l '\"dcmipp_main_postproc\":1->\"dcmipp_main_capture\":0[1]'") != 0) return false;

    if (run_cmd(md + "--set-v4l2 '\"imx335 1-001a\":0[fmt:" + raw_fmt.str() + "]'") != 0) return false;
    if (run_cmd(md + "--set-v4l2 '\"48020000.csi\":0[fmt:" + raw_fmt.str() + "]'") != 0) return false;
    if (run_cmd(md + "--set-v4l2 '\"48020000.csi\":1[fmt:" + raw_fmt.str() + "]'") != 0) return false;
    if (run_cmd(md + "--set-v4l2 '\"dcmipp_input\":0[fmt:" + raw_fmt.str() + "]'") != 0) return false;
    if (run_cmd(md + "--set-v4l2 '\"dcmipp_input\":2[fmt:" + raw_fmt.str() + "]'") != 0) return false;
    if (run_cmd(md + "--set-v4l2 '\"dcmipp_main_isp\":0[fmt:" + raw_fmt.str() + cc.str() + "]'") != 0) return false;
    if (run_cmd(md + "--set-v4l2 '\"dcmipp_main_isp\":1[fmt:" + rgb_fmt.str() + "]'") != 0) return false;
    if (run_cmd(md + "--set-v4l2 '\"dcmipp_main_postproc\":0[fmt:" + rgb_fmt.str() + cc.str() + "]'") != 0) return false;
    if (run_cmd(md + "--set-v4l2 '\"dcmipp_main_postproc\":1[fmt:" + rgb_fmt.str() + "]'") != 0) return false;

    // 传感器增益/曝光: 黑屏与否的关键(无 libcamera 3A). 动态解析 imx335
    // 的 subdev 节点再设置, 避免编号漂移导致设置落空
    std::string sensor = find_subdev("imx335");
    if (!sensor.empty()) {
        (void)system(("v4l2-ctl -d " + sensor + " -c analogue_gain=35 2>/dev/null").c_str());
        (void)system(("v4l2-ctl -d " + sensor + " -c exposure=2000 2>/dev/null").c_str());
        std::cout << "Sensor: " << sensor << " (gain=35, exposure=2000)" << std::endl;
    } else {
        std::cerr << "WARN: imx335 subdev not found, sensor gain/exposure NOT set"
                  << std::endl;
    }
    std::string isp_node = find_subdev("dcmipp_main_isp");
    if (!isp_node.empty())
        (void)system(("v4l2-ctl -d " + isp_node + " -c gamma_correction=1 2>/dev/null").c_str());

    std::cout << "DCMIPP pipeline configured: " << width << "x" << height << std::endl;
    return true;
}

// ====== V4L2 采集: 每个缓冲区只在初始化时 mmap 一次, 热路径零映射开销 ======
struct CaptureBuffer {
    void  *start;
    size_t length;
};

static int open_capture(const char *dev, uint32_t width, uint32_t height,
                        std::vector<CaptureBuffer> &bufs, uint32_t &in_stride) {
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int fd = open(dev, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
        std::cerr << "Cannot open " << dev << ": " << strerror(errno) << std::endl;
        return -1;
    }

    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
        !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        std::cerr << dev << " is not a streaming video capture device" << std::endl;
        close(fd);
        return -1;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        std::cerr << "VIDIOC_S_FMT failed: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }
    if (fmt.fmt.pix.width != width || fmt.fmt.pix.height != height ||
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB24) {
        std::cerr << "Device does not support RGB24 " << width << "x" << height
                  << " (got " << fmt.fmt.pix.width << "x" << fmt.fmt.pix.height << ")"
                  << std::endl;
        close(fd);
        return -1;
    }
    // 行跨距: RGB24 可能有对齐填充, 内核按此索引而不是想当然的 width*3
    in_stride = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline : width * 3;
    std::cout << "Capture: " << dev << " RGB24 " << width << "x" << height
              << ", stride=" << in_stride << std::endl;

    memset(&req, 0, sizeof(req));
    req.count = CAP_BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        std::cerr << "VIDIOC_REQBUFS failed: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }

    bufs.resize(req.count);
    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer q;
        memset(&q, 0, sizeof(q));
        q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        q.memory = V4L2_MEMORY_MMAP;
        q.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &q) < 0) {
            std::cerr << "VIDIOC_QUERYBUF failed: " << strerror(errno) << std::endl;
            close(fd);
            return -1;
        }
        bufs[i].length = q.length;
        bufs[i].start = mmap(NULL, q.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, q.m.offset);
        if (bufs[i].start == MAP_FAILED) {
            std::cerr << "mmap buffer " << i << " failed" << std::endl;
            close(fd);
            return -1;
        }
        if (ioctl(fd, VIDIOC_QBUF, &q) < 0) {
            std::cerr << "VIDIOC_QBUF failed: " << strerror(errno) << std::endl;
            close(fd);
            return -1;
        }
    }

    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        std::cerr << "VIDIOC_STREAMON failed: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }
    return fd;
}

static void close_capture(int fd, std::vector<CaptureBuffer> &bufs) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (fd < 0) return;
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (size_t i = 0; i < bufs.size(); i++) {
        if (bufs[i].start && bufs[i].start != MAP_FAILED)
            munmap(bufs[i].start, bufs[i].length);
    }
    bufs.clear();
    close(fd);
}

// poll 等帧: 返回 1=取到帧(buf填充), 0=暂时无帧, -1=错误
static int dq_frame(int fd, struct v4l2_buffer &buf) {
    struct pollfd pfd;
    int ret;
    pfd.fd = fd;
    pfd.events = POLLIN;
    ret = poll(&pfd, 1, 2000);
    if (ret < 0) return (errno == EINTR) ? 0 : -1;
    if (ret == 0) return 0;                 // 超时, 无帧
    if (pfd.revents & (POLLERR | POLLNVAL)) return -1;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0)
        return (errno == EAGAIN) ? 0 : -1;
    return 1;
}

static int q_buf(int fd, uint32_t index) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    return ioctl(fd, VIDIOC_QBUF, &buf);
}

// ====== OpenCL 内核源码 ======
static std::string readKernelFile(const std::string &filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open kernel file: " << filename << std::endl;
        return "";
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// ====== 内嵌 GStreamer 硬件编码推流管线 ======
// VC8000E 走 V4L2 无状态编码 uAPI, v4l2slh264enc 是其配套用户态实现
// (含用户态 SPS/PPS 生成与码控), 自研客户端代价过高, 这里在程序内复用.
// 用 appsrc 而不是管道+fdsrc: 管道容量 64KB 会让 fdsrc 产生短缓冲,
// 编码器收到声称 1080p 实则 64KB 的缓冲会崩溃; appsrc 由我们构造
// 精确一帧大小且带 GstVideoMeta 的缓冲, 并省掉一次 3MB/帧的管道拷贝.
static GstElement *build_pipeline(const char *ip, int port, uint32_t width,
                                  uint32_t height, int bitrate_kbps, int fps,
                                  GstElement **appsrc_out, GstBus **bus_out) {
    GError *gerr = nullptr;
    GstElement *pipeline = nullptr;
    GstElement *appsrc = nullptr;
    GstCaps *caps = nullptr;
    std::ostringstream desc, caps_str;

    // videoconvert 作隔离层: 进/出 caps 完全一致走直通模式(近零开销),
    // 但它会亲自应答源元素(appsrc/basesrc)发来的 peer query,
    // 避开 v4l2slh264enc pad 查询处理中的 use-after-free 崩溃
    // (实测: filesrc/fdsrc/appsrc 直连编码器必崩, 中间隔 videoconvert 则稳定)
    desc << "appsrc name=frame_src"
         << " ! videoconvert"
         << " ! video/x-raw,format=NV12,width=" << width << ",height=" << height
         << ",framerate=" << fps << "/1"
         << " ! v4l2slh264enc bitrate=" << (bitrate_kbps * 1000)
         << " keyframe-interval=" << fps
         << " ! h264parse config-interval=1"
         << " ! rtph264pay mtu=1400 config-interval=1 pt=96 aggregate-mode=zero-latency"
         << " ! udpsink host=" << ip << " port=" << port << " sync=false async=false";
    std::cout << "Pipeline: " << desc.str() << std::endl;

    pipeline = gst_parse_launch(desc.str().c_str(), &gerr);
    if (!pipeline) {
        std::cerr << "gst_parse_launch failed: "
                  << (gerr ? gerr->message : "unknown") << std::endl;
        if (gerr) g_error_free(gerr);
        return nullptr;
    }
    appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "frame_src");
    if (!appsrc) {
        std::cerr << "appsrc 'frame_src' not found in pipeline" << std::endl;
        gst_object_unref(pipeline);
        return nullptr;
    }

    caps_str << "video/x-raw,format=NV12,width=" << width << ",height=" << height
             << ",framerate=" << fps << "/1";
    caps = gst_caps_from_string(caps_str.str().c_str());
    gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
    gst_caps_unref(caps);
    g_object_set(appsrc,
                 "is-live", (gboolean)TRUE,
                 "block", (gboolean)TRUE,
                 "do-timestamp", (gboolean)TRUE,
                 "format", (gint)GST_FORMAT_TIME,
                 "max-bytes", (guint64)((size_t)width * height * 3 / 2 * 2),
                 nullptr);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Failed to set pipeline to PLAYING" << std::endl;
        gst_object_unref(appsrc);
        gst_object_unref(pipeline);
        return nullptr;
    }

    // 生成接收端 SDP 文件, 播放端无需手工配置
    {
        std::ofstream sdp("stream.sdp");
        sdp << "v=0\n"
            << "o=- 0 0 IN IP4 127.0.0.1\n"
            << "s=rgb_to_nv12\n"
            << "c=IN IP4 127.0.0.1\n"
            << "t=0 0\n"
            << "m=video " << port << " RTP/AVP 96\n"
            << "a=rtpmap:96 H264/90000\n";
    }
    std::cout << "Receiver(PC): ffplay -protocol_whitelist file,udp,rtp "
                 "-fflags nobuffer -flags low_delay -framedrop stream.sdp"
              << std::endl;

    *appsrc_out = appsrc;                  // 调用方持有此引用
    *bus_out = gst_element_get_bus(pipeline);
    return pipeline;
}

// 构造"精确一帧 + GstVideoMeta"的缓冲推入 appsrc, 返回 0 成功
static int push_frame(GstElement *appsrc, const uint8_t *data, size_t size,
                      uint32_t width, uint32_t height) {
    GstBuffer *buf = gst_buffer_new_allocate(nullptr, size, nullptr);
    if (!buf) return -1;
    gst_buffer_fill(buf, 0, data, size);
    gst_buffer_add_video_meta(buf, (GstVideoFrameFlags)0,
                              GST_VIDEO_FORMAT_NV12, (guint)width, (guint)height);
    return (gst_app_src_push_buffer(GST_APP_SRC(appsrc), buf) == GST_FLOW_OK)
               ? 0 : -1;
}

// 非阻塞轮询管线总线, 返回 0 正常, -1 管线出错(无需 GLib 主循环)
static int check_bus(GstBus *bus) {
    GstMessage *msg;
    while ((msg = gst_bus_pop(bus)) != nullptr) {
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

// ====== main ======
int main(int argc, char *argv[]) {
    // ------ 所有 main 作用域变量先于任何 goto 声明并初始化 ------
    const char *dev = (argc > 1) ? argv[1] : CAMERA_DEVICE;
    const char *dest_ip = (argc > 2) ? argv[2] : DEST_IP;
    int dest_port = (argc > 3) ? atoi(argv[3]) : DEST_PORT;
    int bitrate_kbps = (argc > 4) ? atoi(argv[4]) : BITRATE_KBPS;

    const uint32_t width = VIDEO_WIDTH;
    const uint32_t height = VIDEO_HEIGHT;
    const uint32_t in_stride_def = width * 3;   // 仅作初值, open_capture 后用驱动报告值
    uint32_t in_stride = in_stride_def;
    const size_t input_size = (size_t)width * height * 3;
    const size_t output_size = (size_t)width * height * 3 / 2;
    const size_t y_stride = width;              // NV12: Y/UV 行跨距(输出为紧凑布局)

    int cap_fd = -1;
    cl_int err = CL_SUCCESS;
    cl_platform_id platform = nullptr;
    cl_device_id device = nullptr;
    cl_context context = nullptr;
    cl_command_queue queue = nullptr;
    cl_program program = nullptr;
    cl_kernel kernel = nullptr;
    cl_mem input_buffer = nullptr;
    cl_mem out_buffer[2] = {nullptr, nullptr};
    cl_event pending_event = nullptr;
    int pending_slot = -1;
    GstElement *gst_pipeline = nullptr;
    GstElement *appsrc = nullptr;
    GstBus *gst_bus = nullptr;

    std::vector<CaptureBuffer> cap_bufs;
    std::vector<uint8_t> rgb_frame(input_size);
    std::vector<uint8_t> nv12_frame[2] = {
        std::vector<uint8_t>(output_size),
        std::vector<uint8_t>(output_size)
    };
    std::string kernel_source;
    const char *src = nullptr;
    size_t src_len = 0;
    size_t global_work_size[2] = {width / 2, height / 2};
    struct v4l2_buffer cap_buf;
    size_t copy_len = 0;
    int slot = 0;
    bool dumped = false;
    int r = 0;
    uint32_t frame_count = 0;
    auto start_time = std::chrono::steady_clock::now();

    if (width % 2 != 0 || height % 2 != 0) {
        std::cerr << "Width/height must be even (2x2 block kernel)" << std::endl;
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // ------ 1. 配置 DCMIPP media 管线 (重启后会复位, 必须先于采集) ------
    if (!setup_dcmipp_pipeline(width, height)) {
        std::cerr << "Failed to configure DCMIPP media pipeline" << std::endl;
        return 1;
    }

    // ------ 2. V4L2 采集初始化 ------
    cap_fd = open_capture(dev, width, height, cap_bufs, in_stride);
    if (cap_fd < 0) {
        std::cerr << "Failed to setup V4L2 capture" << std::endl;
        return 1;
    }

    // ------ 3. OpenCL 初始化 ------
    err = clGetPlatformIDs(1, &platform, nullptr);
    CHECK_CL_ERROR(err, "clGetPlatformIDs");

    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, nullptr);
    if (err == CL_DEVICE_NOT_FOUND) {
        std::cout << "No GPU found, falling back to CPU." << std::endl;
        err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_CPU, 1, &device, nullptr);
    }
    CHECK_CL_ERROR(err, "clGetDeviceIDs");

    context = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
    CHECK_CL_ERROR(err, "clCreateContext");

#ifdef CL_VERSION_2_0
    queue = clCreateCommandQueueWithProperties(context, device, nullptr, &err);
#else
    queue = clCreateCommandQueue(context, device, 0, &err);
#endif
    CHECK_CL_ERROR(err, "clCreateCommandQueue");

    kernel_source = readKernelFile("rgb_to_nv12.cl");
    if (kernel_source.empty()) {
        std::cerr << "Failed to read kernel file" << std::endl;
        goto cleanup;
    }
    src = kernel_source.c_str();
    src_len = kernel_source.length();
    program = clCreateProgramWithSource(context, 1, &src, &src_len, &err);
    CHECK_CL_ERROR(err, "clCreateProgramWithSource");

    err = clBuildProgram(program, 1, &device, "", nullptr, nullptr);
    if (err != CL_SUCCESS) {
        size_t log_size = 0;
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
        std::vector<char> log(log_size + 1);
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, log_size, log.data(), nullptr);
        std::cerr << "Kernel build error: " << log.data() << std::endl;
        goto cleanup;
    }
    kernel = clCreateKernel(program, "rgb888_to_nv12", &err);
    CHECK_CL_ERROR(err, "clCreateKernel");

    // 输入: COPY_HOST_PTR 创建 + 每帧 clEnqueueWriteBuffer 显式搬运.
    // 不能用 USE_HOST_PTR: Vivante 等嵌入式 GPU 上 CPU 写/GPU 直接读存在
    // 缓存一致性风险(GPU 读到初始旧值, 整帧纯色 -> 推流全黑).
    input_buffer = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                  input_size, rgb_frame.data(), &err);
    CHECK_CL_ERROR(err, "clCreateBuffer (input)");
    out_buffer[0] = clCreateBuffer(context, CL_MEM_WRITE_ONLY, output_size, nullptr, &err);
    CHECK_CL_ERROR(err, "clCreateBuffer (output 0)");
    out_buffer[1] = clCreateBuffer(context, CL_MEM_WRITE_ONLY, output_size, nullptr, &err);
    CHECK_CL_ERROR(err, "clCreateBuffer (output 1)");

    err = clSetKernelArg(kernel, 0, sizeof(cl_mem), &input_buffer);
    CHECK_CL_ERROR(err, "clSetKernelArg (input)");
    err = clSetKernelArg(kernel, 2, sizeof(cl_uint), &width);
    CHECK_CL_ERROR(err, "clSetKernelArg (width)");
    err = clSetKernelArg(kernel, 3, sizeof(cl_uint), &height);
    CHECK_CL_ERROR(err, "clSetKernelArg (height)");
    err = clSetKernelArg(kernel, 4, sizeof(cl_uint), &in_stride);
    CHECK_CL_ERROR(err, "clSetKernelArg (in_stride)");
    err = clSetKernelArg(kernel, 5, sizeof(cl_uint), &y_stride);
    CHECK_CL_ERROR(err, "clSetKernelArg (y_stride)");
    // arg 1 (输出缓冲) 每帧切换, 在主循环中设置

    // ------ 4. 启动硬件编码推流管线 ------
    gst_init(nullptr, nullptr);
    gst_pipeline = build_pipeline(dest_ip, dest_port, width, height,
                                  bitrate_kbps, PIPELINE_FPS, &appsrc, &gst_bus);
    if (!gst_pipeline) goto cleanup;

    // ------ 5. 主循环: poll 采集 -> GPU 转换(异步) -> 喂给编码管线 ------
    std::cout << "Streaming " << dev << " -> " << dest_ip << ":" << dest_port
              << " (Ctrl-C to stop)" << std::endl;
    start_time = std::chrono::steady_clock::now();

    while (!g_stop) {
        r = dq_frame(cap_fd, cap_buf);
        if (r < 0) {
            std::cerr << "Capture error, exiting" << std::endl;
            break;
        }
        if (r == 0) continue;    // 无帧, 回到 poll 休眠

        // 取到一帧: 先拷贝出来, 立即归还缓冲区给驱动
        copy_len = std::min((size_t)cap_buf.bytesused, input_size);
        memcpy(rgb_frame.data(), cap_bufs[cap_buf.index].start, copy_len);
        if (q_buf(cap_fd, cap_buf.index) < 0) {
            std::cerr << "Failed to requeue capture buffer" << std::endl;
            break;
        }

        // 上一帧 GPU 转换已完成与否: 等待其 event, 把 NV12 结果送入编码管线.
        // 此时 GPU 正在算的本帧与本行之后的采集天然重叠.
        if (pending_event) {
            err = clWaitForEvents(1, &pending_event);
            clReleaseEvent(pending_event);
            pending_event = nullptr;
            if (err != CL_SUCCESS) {
                std::cerr << "clWaitForEvents error: " << err << std::endl;
                break;
            }
            if (check_bus(gst_bus) != 0) break;
            if (!dumped) {
                // 首帧 NV12 落盘, 便于用 ffmpeg 核验 GPU 转换结果
                dumped = true;
                FILE *df = fopen("nv12_debug.bin", "wb");
                if (df) {
                    fwrite(nv12_frame[pending_slot].data(), 1, output_size, df);
                    fclose(df);
                    std::cout << "Dumped first NV12 frame: nv12_debug.bin" << std::endl;
                }
            }
            if (push_frame(appsrc, nv12_frame[pending_slot].data(), output_size,
                           width, height) != 0) {
                std::cerr << "Push frame to encoder pipeline failed" << std::endl;
                break;
            }
        }

        // 提交本帧转换: 写入设备内存 -> kernel -> 非阻塞回读, 立即返回继续采集下一帧
        slot = (int)(frame_count & 1);
        err = clEnqueueWriteBuffer(queue, input_buffer, CL_FALSE, 0,
                                   input_size, rgb_frame.data(),
                                   0, nullptr, nullptr);
        CHECK_CL_ERROR(err, "clEnqueueWriteBuffer");
        err = clSetKernelArg(kernel, 1, sizeof(cl_mem), &out_buffer[slot]);
        CHECK_CL_ERROR(err, "clSetKernelArg (output)");
        err = clEnqueueNDRangeKernel(queue, kernel, 2, nullptr, global_work_size,
                                     nullptr, 0, nullptr, nullptr);
        CHECK_CL_ERROR(err, "clEnqueueNDRangeKernel");
        err = clEnqueueReadBuffer(queue, out_buffer[slot], CL_FALSE, 0,
                                  output_size, nv12_frame[slot].data(),
                                  0, nullptr, &pending_event);
        CHECK_CL_ERROR(err, "clEnqueueReadBuffer");
        pending_slot = slot;

        frame_count++;
        if (frame_count % 30 == 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed > 0)
                std::cout << "Frames: " << frame_count
                          << ", FPS: " << (frame_count * 1000LL / elapsed) << std::endl;
        }
    }

    // 冲刷流水线中最后一帧
    if (pending_event) {
        clWaitForEvents(1, &pending_event);
        clReleaseEvent(pending_event);
        pending_event = nullptr;
        push_frame(appsrc, nv12_frame[pending_slot].data(), output_size,
                   width, height);
    }

cleanup:
    std::cout << "Cleaning up, total frames: " << frame_count << std::endl;
    if (pending_event) clReleaseEvent(pending_event);
    if (gst_pipeline) {
        gst_element_set_state(gst_pipeline, GST_STATE_NULL);
        if (gst_bus) gst_object_unref(gst_bus);
        if (appsrc) gst_object_unref(appsrc);
        gst_object_unref(gst_pipeline);
    }
    if (kernel) clReleaseKernel(kernel);
    if (program) clReleaseProgram(program);
    if (queue) clReleaseCommandQueue(queue);
    if (context) clReleaseContext(context);
    if (input_buffer) clReleaseMemObject(input_buffer);
    if (out_buffer[0]) clReleaseMemObject(out_buffer[0]);
    if (out_buffer[1]) clReleaseMemObject(out_buffer[1]);
    close_capture(cap_fd, cap_bufs);
    return 0;
}
