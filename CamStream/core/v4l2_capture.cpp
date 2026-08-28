#include "v4l2_capture.h"

#include <iostream>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

namespace camstream {

// ====== V4L2 采集: 每个缓冲区只在初始化时 mmap 一次, 热路径零映射开销 ======
int open_capture(const char *dev, uint32_t width, uint32_t height, int buf_count,
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
    req.count = buf_count;
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

void close_capture(int fd, std::vector<CaptureBuffer> &bufs) {
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
int dq_frame(int fd, struct v4l2_buffer &buf) {
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

int q_buf(int fd, uint32_t index) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    return ioctl(fd, VIDIOC_QBUF, &buf);
}

// 连续取 N 帧计时实测采集帧率(数据丢弃), 返回 0 表示探测失败.
// 先丢弃 WARMUP 帧预热: STREAMON/模式切换后的头几帧间隔不稳定, 会拉低均值.
double probe_capture_fps(int cap_fd) {
    const int WARMUP = 10, N = 20;
    struct v4l2_buffer buf;
    int got = 0, timeouts = 0;
    for (int i = 0; i < WARMUP; ) {
        int r = dq_frame(cap_fd, buf);
        if (r < 0) return 0;
        if (r == 0) {
            if (++timeouts > 3) return 0;   // 相机停流, 放弃探测
            continue;
        }
        timeouts = 0;
        if (q_buf(cap_fd, buf.index) < 0) return 0;
        i++;
    }
    timeouts = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (got < N) {
        int r = dq_frame(cap_fd, buf);
        if (r < 0) return 0;
        if (r == 0) {
            if (++timeouts > 3) return 0;
            continue;
        }
        timeouts = 0;
        if (q_buf(cap_fd, buf.index) < 0) return 0;
        got++;
    }
    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    return (ms > 0.0) ? (N * 1000.0 / ms) : 0.0;
}

} // namespace camstream
