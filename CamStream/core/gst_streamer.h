/*
 * gst_streamer.h - 程序内构建的 GStreamer 硬件编码推流管线
 *
 * 从原 main 逐字搬运(Phase 1 行为不变):
 *   - build()    : appsrc ! videoconvert(直通隔离) ! v4l2slh264enc(VC8000E)
 *                  ! h264parse ! rtph264pay(zero-latency) ! udpsink
 *                  + PLAYING + SDP 文件生成 + 接收端命令打印
 *   - pushFrame(): 构造"精确一帧 + GstVideoMeta"缓冲推入 appsrc
 *   - checkBus() : 非阻塞轮询管线总线(无需 GLib 主循环)
 *   - ~GstStreamer(): 镜像原 cleanup(STATE_NULL + unref bus/appsrc/pipeline)
 *
 * videoconvert 作隔离层: 进/出 caps 完全一致走直通(近零开销), 但它会亲自
 * 应答源元素发来的 peer query, 避开 v4l2slh264enc pad 查询处理中的
 * use-after-free 崩溃(实测: appsrc 直连编码器必崩, 中间隔 videoconvert 稳定).
 */

#ifndef CAMSTREAM_GST_STREAMER_H
#define CAMSTREAM_GST_STREAMER_H

#include <cstdint>
#include <cstddef>

// GStreamer: appsrc 注入 + 硬件编码推流
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

namespace camstream {

class GstStreamer {
public:
    GstStreamer();
    ~GstStreamer();

    // 构建 pipeline 并置 PLAYING, 生成 sdp_file, 打印接收端命令.
    // gop: keyframe-interval; <=0 时保持原公式(fps<10?10:fps, 约 1 IDR/秒)
    // colorimetry: 0=bt601(默认) 1=bt709; 同时作用于 caps filter 与 appsrc
    // caps, 须与 OpenCL 内核矩阵一致(stream_app 负责配套传入)
    // max_queue_frames: appsrc max-bytes = 单帧大小*该值(默认 1, 低延迟:
    //   编码链未跟上时 push 阻塞, 延迟被结构性限制在 ~1 帧; 调大将提高
    //   吞吐平滑度但增加排队延迟, 仅用于 A/B 实验)
    // 返回 false 时半建资源由析构清理.
    bool build(const char *ip, int port, uint32_t width, uint32_t height,
               int bitrate_kbps, int fps, const char *sdp_file,
               int gop = 0, int colorimetry = 0, int max_queue_frames = 1);

    // 推入完整一帧(带 GstVideoMeta), 返回 0 成功 / -1 失败
    int pushFrame(const uint8_t *data, size_t size, uint32_t width, uint32_t height);

    // 非阻塞轮询管线总线, 返回 0 正常, -1 管线出错
    int checkBus();

    // appsrc 当前积压字节数(诊断用; 属性读取, 仅应在状态刷新节拍调用,
    // 不放进每帧热路径). 除以单帧大小即为积压帧数.
    uint64_t queuedBytes() const;

    bool ready() const { return pipeline_ != nullptr; }

    // 立即置 NULL 并释放管线(幂等, 析构函数再调一次无副作用).
    // 需在 close_capture 之前调用, 与原 main cleanup 顺序一致.
    void destroy();

private:
    GstStreamer(const GstStreamer &);
    GstStreamer &operator=(const GstStreamer &);

    GstElement *pipeline_;
    GstElement *appsrc_;
    GstBus *bus_;
};

} // namespace camstream

#endif // CAMSTREAM_GST_STREAMER_H
