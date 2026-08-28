/*
 * v4l2_capture.h - V4L2 MMAP 采集
 *
 * 从原 main 逐字搬运(Phase 1 行为不变):
 *   - CaptureBuffer    : mmap 缓冲描述(只在初始化时映射一次, 热路径零映射开销)
 *   - open_capture     : open/S_FMT(RGB24)/REQBUFS/mmap/QBUF/STREAMON, 返回 fd
 *   - close_capture    : STREAMOFF/munmap/close
 *   - dq_frame         : poll + DQBUF, 返回 1=取到帧 0=暂时无帧 -1=错误
 *   - q_buf            : 归还缓冲
 *   - probe_capture_fps: 连续取 N 帧计时实测采集帧率(数据丢弃)
 *
 * 与原实现唯一差异: 缓冲区个数由宏 CAP_BUF_COUNT 改为 open_capture 参数
 * (值仍来自 AppConfig.cap_buf_count, 两档均为 4, 行为不变).
 */

#ifndef CAMSTREAM_V4L2_CAPTURE_H
#define CAMSTREAM_V4L2_CAPTURE_H

#include <vector>
#include <cstdint>
#include <cstddef>
#include <linux/videodev2.h>

namespace camstream {

struct CaptureBuffer {
    void  *start;
    size_t length;
};

int open_capture(const char *dev, uint32_t width, uint32_t height, int buf_count,
                 std::vector<CaptureBuffer> &bufs, uint32_t &in_stride);
void close_capture(int fd, std::vector<CaptureBuffer> &bufs);

// poll 等帧: 返回 1=取到帧(buf填充), 0=暂时无帧, -1=错误
int dq_frame(int fd, struct v4l2_buffer &buf);

int q_buf(int fd, uint32_t index);

// 连续取 N 帧计时实测采集帧率(数据丢弃), 返回 0 表示探测失败.
// 先丢弃 WARMUP 帧预热: STREAMON/模式切换后的头几帧间隔不稳定, 会拉低均值.
double probe_capture_fps(int cap_fd);

} // namespace camstream

#endif // CAMSTREAM_V4L2_CAPTURE_H
