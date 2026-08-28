/*
 * RGB24(摄像头) --OpenCL(GPU)--> NV12 --appsrc--> GStreamer 硬件H264编码 --UDP--> RTP 推流
 * 模式: 1080p @ 12fps (传感器按 12fps 节拍运行, 流元数据与实际交付一致)
 *
 * 数据通路:
 *   启动时自动用 media-ctl 重建 DCMIPP 链路并设置各级格式与传感器
 *   增益/曝光(重启后内核复位为 640x480, 不重配则 STREAMON 失败)
 *   /dev/video1 (DCMIPP, RGB888_1X24 1920x1080)
 *     -> poll + DQBUF (mmap 缓冲区只在初始化时映射一次, 热路径零映射开销)
 *     -> OpenCL 内核经 USE_HOST_PTR 直接读 V4L2 mmap 缓冲(零拷贝):
 *        V4L2 缓冲是非缓存的 DMA 内存, GPU DMA 读取天然一致;
 *        (注意: 此前用 malloc 缓冲 + USE_HOST_PTR 曾黑屏, 那是 CPU cache
 *         未刷导致; 且 CPU 从非缓存内存 memcpy 极慢(1080p 每帧 38ms),
 *         本方案把这笔拷贝与显式 WriteBuffer 一并省掉)
 *     -> RGB888->NV12 (每 work-item 处理 2x2 像素块, 显式 local size 16x4)
 *     -> clEnqueueReadBuffer 回读 + event, 双缓冲流水线
 *     -> appsrc 注入完整一帧(带 GstVideoMeta)到程序内构建的 GStreamer 管线:
 *        appsrc ! videoconvert(直通隔离, 避开 v4l2slh264enc pad 查询崩溃)
 *          ! v4l2slh264enc(VC8000E 硬件编码) ! h264parse ! rtph264pay ! udpsink
 *     启动时生成 stream_1080p.sdp, PC 端 ffplay/VLC 按其播放
 *
 * 用法: ./rgb_1080p [采集设备] [目标IP] [端口] [码率kbps] [帧率]
 * 默认: /dev/video1 192.168.137.1 5000 4000 12
 * 帧率参数>0 时调节传感器帧间隔(vblank)并作为流元数据(以实测校准);
 * 1080p 模式处理链实测上限约 20fps, 默认 12fps 为稳定工作点
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
#define CAMERA_DEVICE   "/dev/video1"     // DCMIPP main postproc 输出节点
#define SENSOR_WIDTH    1920              // 传感器->CSI->ISP 链路分辨率
#define SENSOR_HEIGHT   1080
#define OUT_WIDTH       1920              // postproc 输出 = 采集/GPU/编码分辨率
#define OUT_HEIGHT      1080              // (内核按 2x2 块处理, 要求为偶数)
#define DEST_IP         "192.168.1.100"   // 接收主机IP
#define DEST_PORT       5000
#define BITRATE_KBPS    4000              // H264 目标码率
#define DEFAULT_FPS     12                // 默认帧率(1080p 稳定工作点)
#define TARGET_FPS      12                // 探测失败时的兜底帧率
#define CAP_BUF_COUNT   4                 // V4L2 采集缓冲区个数
#define SDP_FILE        "stream_1080p.sdp"

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
// 传感器->CSI->input 为 SRGGB10, ISP 输出起为 RGB888_1X24;
// postproc crop 全幅、compose 到输出分辨率(输出小于传感器时由其硬件缩放).
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

static bool setup_dcmipp_pipeline(uint32_t sensor_w, uint32_t sensor_h,
                                  uint32_t out_w, uint32_t out_h,
                                  std::string &sensor_out) {
    const std::string md = "media-ctl -d platform:48030000.dcmipp ";
    std::ostringstream raw_fmt, rgb_isp_fmt, rgb_out_fmt, cc_isp, cc_out;

    raw_fmt << "SRGGB10_1X10/" << sensor_w << "x" << sensor_h;
    rgb_isp_fmt << "RGB888_1X24/" << sensor_w << "x" << sensor_h;
    rgb_out_fmt << "RGB888_1X24/" << out_w << "x" << out_h;
    cc_isp << " crop:(0,0)/" << sensor_w << "x" << sensor_h
           << " compose:(0,0)/" << sensor_w << "x" << sensor_h;
    cc_out << " crop:(0,0)/" << sensor_w << "x" << sensor_h
           << " compose:(0,0)/" << out_w << "x" << out_h;

    // 清理可能占用设备的残留 gst 进程
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
    if (run_cmd(md + "--set-v4l2 '\"dcmipp_main_isp\":0[fmt:" + raw_fmt.str() + cc_isp.str() + "]'") != 0) return false;
    if (run_cmd(md + "--set-v4l2 '\"dcmipp_main_isp\":1[fmt:" + rgb_isp_fmt.str() + "]'") != 0) return false;
    if (run_cmd(md + "--set-v4l2 '\"dcmipp_main_postproc\":0[fmt:" + rgb_isp_fmt.str() + cc_out.str() + "]'") != 0) return false;
    if (run_cmd(md + "--set-v4l2 '\"dcmipp_main_postproc\":1[fmt:" + rgb_out_fmt.str() + "]'") != 0) return false;

    // 传感器增益/曝光: 黑屏与否的关键(无 libcamera 3A). 动态解析 imx335
    // 的 subdev 节点再设置, 避免编号漂移导致设置落空
    std::string sensor = find_subdev("imx335");
    sensor_out = sensor;
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

    std::cout << "DCMIPP pipeline configured: sensor " << sensor_w << "x" << sensor_h
              << " -> output " << out_w << "x" << out_h << std::endl;
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

// 连续取 N 帧计时实测采集帧率(数据丢弃), 返回 0 表示探测失败.
// 先丢弃 WARMUP 帧预热: STREAMON/模式切换后的头几帧间隔不稳定, 会拉低均值.
static double probe_capture_fps(int cap_fd) {
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

// 按目标帧率调节传感器帧间隔(尽力而为, 失败仅告警不阻塞).
// fps = pixel_rate / ((sensor_w + hblank) * (sensor_h + vblank)), 可调项只有 vblank.
// 注意宽高用传感器侧尺寸(720p 模式下输出分辨率与此不同).
static void set_sensor_fps(const std::string &subdev, uint32_t sensor_w,
                           uint32_t sensor_h, int fps) {
    int fd = open(subdev.c_str(), O_RDWR);
    if (fd < 0) {
        std::cerr << "WARN: open " << subdev << " for fps setting failed" << std::endl;
        return;
    }

    // 当前 hblank / vblank
    struct v4l2_ext_control ec[2];
    struct v4l2_ext_controls ecs;
    memset(ec, 0, sizeof(ec));
    ec[0].id = V4L2_CID_HBLANK;
    ec[1].id = V4L2_CID_VBLANK;
    memset(&ecs, 0, sizeof(ecs));
    ecs.ctrl_class = V4L2_CTRL_CLASS_IMAGE_SOURCE;
    ecs.count = 2;
    ecs.controls = ec;
    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &ecs) < 0) {
        std::cerr << "WARN: read sensor blanking failed: " << strerror(errno) << std::endl;
        close(fd);
        return;
    }
    const int64_t hblank = ec[0].value;
    const int64_t vblank = ec[1].value;

    // pixel_rate 为 int64 控制
    memset(ec, 0, sizeof(ec));
    ec[0].id = V4L2_CID_PIXEL_RATE;
    memset(&ecs, 0, sizeof(ecs));
    ecs.ctrl_class = V4L2_CTRL_CLASS_IMAGE_PROC;
    ecs.count = 1;
    ecs.controls = ec;
    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &ecs) < 0) {
        std::cerr << "WARN: read pixel_rate failed: " << strerror(errno) << std::endl;
        close(fd);
        return;
    }
    const int64_t pixel_rate = ec[0].value64;

    // vblank 允许范围
    struct v4l2_query_ext_ctrl qc;
    memset(&qc, 0, sizeof(qc));
    qc.id = V4L2_CID_VBLANK;
    if (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &qc) < 0) {
        std::cerr << "WARN: query vblank range failed: " << strerror(errno) << std::endl;
        close(fd);
        return;
    }

    const double cur_fps = (double)pixel_rate /
        ((double)(sensor_w + hblank) * (double)(sensor_h + vblank));
    int64_t want = pixel_rate / ((int64_t)(sensor_w + hblank) * fps) - sensor_h;
    if (want < qc.minimum) want = qc.minimum;
    if (want > qc.maximum) want = qc.maximum;
    const double new_fps = (double)pixel_rate /
        ((double)(sensor_w + hblank) * (double)(sensor_h + want));

    if (want == vblank) {
        std::cout << "Sensor fps: " << cur_fps << " (vblank " << vblank
                  << " already at target)" << std::endl;
        close(fd);
        return;
    }

    memset(ec, 0, sizeof(ec));
    ec[0].id = V4L2_CID_VBLANK;
    ec[0].value = (int32_t)want;
    memset(&ecs, 0, sizeof(ecs));
    ecs.ctrl_class = V4L2_CTRL_CLASS_IMAGE_SOURCE;
    ecs.count = 1;
    ecs.controls = ec;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ecs) == 0) {
        // 回读校验: 部分驱动在流水线重配(S_FMT/STREAMON)后会悄悄还原控制值
        memset(ec, 0, sizeof(ec));
        ec[0].id = V4L2_CID_VBLANK;
        memset(&ecs, 0, sizeof(ecs));
        ecs.ctrl_class = V4L2_CTRL_CLASS_IMAGE_SOURCE;
        ecs.count = 1;
        ecs.controls = ec;
        int64_t now_vblank = vblank;
        if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &ecs) == 0)
            now_vblank = ec[0].value;
        if (now_vblank != want)
            std::cerr << "WARN: vblank readback " << now_vblank
                      << " != requested " << want << std::endl;
        std::cout << "Sensor fps: " << cur_fps << " -> " << new_fps
                  << " (vblank " << vblank << " -> " << want << ")" << std::endl;
    }
    else
        std::cerr << "WARN: set vblank failed: " << strerror(errno) << std::endl;
    close(fd);
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
// 用 appsrc 注入完整一帧(带 GstVideoMeta)的缓冲.
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
         << " keyframe-interval=" << (fps < 10 ? 10 : fps)
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
                 "max-bytes", (guint64)((size_t)width * height * 3 / 2),
                 nullptr);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Failed to set pipeline to PLAYING" << std::endl;
        gst_object_unref(appsrc);
        gst_object_unref(pipeline);
        return nullptr;
    }

    // 生成接收端 SDP 文件(连接地址为接收主机, 单播场景即 udpsink 目标)
    {
        std::ofstream sdp(SDP_FILE);
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
                 "-probesize 32768 -analyzeduration 0 " << SDP_FILE << "\n"
              << "             vlc --network-caching=100 " << SDP_FILE
                 "  (rtp:// URI 不支持动态载荷96, 须用 sdp 文件)"
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

// 距 t 时刻的毫秒数(主循环 timing 仪表用)
static double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t).count();
}

// ====== main ======
int main(int argc, char *argv[]) {
    // ------ 所有 main 作用域变量先于任何 goto 声明并初始化 ------
    const char *dev = (argc > 1) ? argv[1] : CAMERA_DEVICE;
    const char *dest_ip = (argc > 2) ? argv[2] : DEST_IP;
    int dest_port = (argc > 3) ? atoi(argv[3]) : DEST_PORT;
    int bitrate_kbps = (argc > 4) ? atoi(argv[4]) : BITRATE_KBPS;
    int target_fps = (argc > 5) ? atoi(argv[5]) : DEFAULT_FPS;

    const uint32_t sensor_width = SENSOR_WIDTH, sensor_height = SENSOR_HEIGHT;
    const uint32_t width = OUT_WIDTH, height = OUT_HEIGHT;   // 输出(采集/GPU/编码)尺寸
    uint32_t in_stride = width * 3;                          // open_capture 后用驱动报告值
    const size_t input_size = (size_t)width * height * 3;
    size_t input_span = input_size;   // 内核实际访问跨度: (H-1)*stride + W*3
    const size_t output_size = (size_t)width * height * 3 / 2;
    const size_t y_stride = width;    // NV12: Y/UV 行跨距(输出为紧凑布局)

    int cap_fd = -1;
    cl_int err = CL_SUCCESS;
    cl_platform_id platform = nullptr;
    cl_device_id device = nullptr;
    cl_context context = nullptr;
    cl_command_queue queue = nullptr;
    cl_program program = nullptr;
    cl_kernel kernel = nullptr;
    std::vector<cl_mem> input_clmem;          // 每个采集缓冲一个, USE_HOST_PTR 零拷贝
    cl_mem out_buffer[2] = {nullptr, nullptr};
    cl_event pending_event = nullptr;
    cl_event kernel_event = nullptr;
    int pending_slot = -1;
    GstElement *gst_pipeline = nullptr;
    GstElement *appsrc = nullptr;
    GstBus *gst_bus = nullptr;

    std::vector<CaptureBuffer> cap_bufs;
    std::vector<uint8_t> nv12_frame[2] = {
        std::vector<uint8_t>(output_size),
        std::vector<uint8_t>(output_size)
    };
    std::string kernel_source;
    std::string sensor_node;
    const char *src = nullptr;
    size_t src_len = 0;
    size_t global_work_size[2] = {width / 2, height / 2};
    size_t local_work_size[2] = {16, 4};   // 显式 local size: 部分驱动自动选择极差
    const size_t *lws = (global_work_size[0] % local_work_size[0] == 0 &&
                         global_work_size[1] % local_work_size[1] == 0)
                            ? local_work_size : nullptr;
    struct v4l2_buffer cap_buf;
    int slot = 0;
    bool dumped = false;
    int r = 0;
    int stream_fps = 0;
    double measured_fps = 0.0;
    double acc_poll = 0.0, acc_gpu = 0.0, acc_push = 0.0;
    double acc_kx = 0.0, acc_rd = 0.0;
    int acc_frames = 0;
    uint32_t frame_count = 0;
    auto start_time = std::chrono::steady_clock::now();

    if (width % 2 != 0 || height % 2 != 0) {
        std::cerr << "Width/height must be even (2x2 block kernel)" << std::endl;
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // ------ 1. 配置 DCMIPP media 管线 (重启后会复位, 必须先于采集) ------
    if (!setup_dcmipp_pipeline(sensor_width, sensor_height, width, height,
                               sensor_node)) {
        std::cerr << "Failed to configure DCMIPP media pipeline" << std::endl;
        return 1;
    }

    // ------ 2. V4L2 采集初始化 ------
    cap_fd = open_capture(dev, width, height, cap_bufs, in_stride);
    if (cap_fd < 0) {
        std::cerr << "Failed to setup V4L2 capture" << std::endl;
        return 1;
    }
    input_span = (size_t)(height - 1) * in_stride + (size_t)width * 3;

    // ------ 3. 帧率: 调传感器帧间隔(流前+流后各一次, 防 S_FMT/STREAMON
    //            复位控制值); 探测实测值作流元数据 ------
    if (target_fps > 0) {
        if (!sensor_node.empty()) {
            set_sensor_fps(sensor_node, sensor_width, sensor_height, target_fps);
            set_sensor_fps(sensor_node, sensor_width, sensor_height, target_fps);
        } else {
            std::cerr << "WARN: sensor node unknown, skip fps setting" << std::endl;
        }
    }
    measured_fps = probe_capture_fps(cap_fd);
    // 实测明显低于目标: 流状态下再设一次并复测
    if (target_fps > 0 && !sensor_node.empty() &&
        measured_fps > 0.0 && measured_fps < 0.85 * target_fps) {
        std::cout << "Measured " << measured_fps << "fps << target " << target_fps
                  << ", retrying sensor fps setting..." << std::endl;
        set_sensor_fps(sensor_node, sensor_width, sensor_height, target_fps);
        measured_fps = probe_capture_fps(cap_fd);
    }
    if (measured_fps <= 0.0)
        measured_fps = (target_fps > 0) ? target_fps : TARGET_FPS;
    stream_fps = (int)(measured_fps + 0.5);
    if (stream_fps < 1) stream_fps = 1;
    if (target_fps > 0 && (double)stream_fps < 0.85 * target_fps)
        std::cerr << "WARN: sensor delivers " << stream_fps << "fps, target "
                  << target_fps << "fps not reached" << std::endl;
    std::cout << "Stream fps: " << stream_fps
              << " (measured " << measured_fps << ")" << std::endl;

    // ------ 4. OpenCL 初始化 ------
    err = clGetPlatformIDs(1, &platform, nullptr);
    CHECK_CL_ERROR(err, "clGetPlatformIDs");

    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, nullptr);
    if (err == CL_DEVICE_NOT_FOUND) {
        std::cout << "No GPU found, falling back to CPU." << std::endl;
        err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_CPU, 1, &device, nullptr);
    }
    CHECK_CL_ERROR(err, "clGetDeviceIDs");

    // 打印实际执行内核的设备(诊断: 确认真的是 GPU 而非 CPU 回退实现)
    {
        char dev_name[128] = {0}, dev_vendor[128] = {0};
        clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(dev_name) - 1, dev_name, nullptr);
        clGetDeviceInfo(device, CL_DEVICE_VENDOR, sizeof(dev_vendor) - 1, dev_vendor, nullptr);
        std::cout << "OpenCL device: " << dev_name << " (" << dev_vendor << ")" << std::endl;
    }

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

    // 输入: 对每个 V4L2 mmap 缓冲建 USE_HOST_PTR 零拷贝 cl_mem.
    // V4L2 缓冲是非缓存 DMA 内存, GPU 直读一致(见文件头注释);
    // 内核访问跨度按 in_stride 计算, 覆盖行对齐填充的情况.
    input_clmem.resize(cap_bufs.size());
    for (size_t i = 0; i < cap_bufs.size(); i++) {
        input_clmem[i] = clCreateBuffer(context,
                                        CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
                                        input_span, cap_bufs[i].start, &err);
        CHECK_CL_ERROR(err, "clCreateBuffer (input USE_HOST_PTR)");
    }
    out_buffer[0] = clCreateBuffer(context, CL_MEM_WRITE_ONLY, output_size, nullptr, &err);
    CHECK_CL_ERROR(err, "clCreateBuffer (output 0)");
    out_buffer[1] = clCreateBuffer(context, CL_MEM_WRITE_ONLY, output_size, nullptr, &err);
    CHECK_CL_ERROR(err, "clCreateBuffer (output 1)");

    // 固定内核参数; arg0(输入, 每帧切换到当前采集缓冲)与 arg1(输出, 双缓冲)
    // 在主循环中按帧设置
    err = clSetKernelArg(kernel, 2, sizeof(cl_uint), &width);
    CHECK_CL_ERROR(err, "clSetKernelArg (width)");
    err = clSetKernelArg(kernel, 3, sizeof(cl_uint), &height);
    CHECK_CL_ERROR(err, "clSetKernelArg (height)");
    err = clSetKernelArg(kernel, 4, sizeof(cl_uint), &in_stride);
    CHECK_CL_ERROR(err, "clSetKernelArg (in_stride)");
    err = clSetKernelArg(kernel, 5, sizeof(cl_uint), &y_stride);
    CHECK_CL_ERROR(err, "clSetKernelArg (y_stride)");

    // ------ 5. 启动硬件编码推流管线 ------
    gst_init(nullptr, nullptr);
    gst_pipeline = build_pipeline(dest_ip, dest_port, width, height,
                                  bitrate_kbps, stream_fps, &appsrc, &gst_bus);
    if (!gst_pipeline) goto cleanup;

    // ------ 6. 主循环: 采集 -> GPU 转换(与推送重叠) -> 回读 ------
    std::cout << "Streaming " << dev << " -> " << dest_ip << ":" << dest_port
              << " (Ctrl-C to stop)" << std::endl;
    start_time = std::chrono::steady_clock::now();

    while (!g_stop) {
        auto t_poll = std::chrono::steady_clock::now();
        r = dq_frame(cap_fd, cap_buf);
        acc_poll += ms_since(t_poll);
        if (r < 0) {
            std::cerr << "Capture error, exiting" << std::endl;
            break;
        }
        if (r == 0) continue;    // 无帧, 回到 poll 休眠

        // 提交本帧 GPU 转换(异步): 先入队, 让 GPU 与下面的推送工作重叠.
        // 采集缓冲在内核读完(kx 等待返回)之前不归还给驱动.
        slot = (int)(frame_count & 1);
        err = clSetKernelArg(kernel, 0, sizeof(cl_mem), &input_clmem[cap_buf.index]);
        CHECK_CL_ERROR(err, "clSetKernelArg (input)");
        err = clSetKernelArg(kernel, 1, sizeof(cl_mem), &out_buffer[slot]);
        CHECK_CL_ERROR(err, "clSetKernelArg (output)");
        err = clEnqueueNDRangeKernel(queue, kernel, 2, nullptr, global_work_size,
                                     lws, 0, nullptr, &kernel_event);
        CHECK_CL_ERROR(err, "clEnqueueNDRangeKernel");

        // 上一帧: 等回读完成, 送入编码管线(GPU 同时在算本帧)
        if (pending_event) {
            auto t_gpu = std::chrono::steady_clock::now();
            err = clWaitForEvents(1, &pending_event);
            acc_gpu += ms_since(t_gpu);
            clReleaseEvent(pending_event);
            pending_event = nullptr;
            if (err != CL_SUCCESS) {
                std::cerr << "clWaitForEvents error: " << err << std::endl;
                break;
            }
            if (!dumped) {
                // 首帧 NV12 落盘 + 均匀度检查: USE_HOST_PTR 一致性失败会得到
                // 均匀黑帧, 在这里立即暴露而不是表现为"推流黑屏"
                dumped = true;
                FILE *df = fopen("nv12_debug.bin", "wb");
                if (df) {
                    fwrite(nv12_frame[pending_slot].data(), 1, output_size, df);
                    fclose(df);
                    std::cout << "Dumped first NV12 frame: nv12_debug.bin" << std::endl;
                }
                const uint8_t *yp = nv12_frame[pending_slot].data();
                size_t ysize = output_size * 2 / 3;
                uint8_t ymin = 255, ymax = 0;
                for (size_t i = 0; i < ysize; i += 1024) {
                    if (yp[i] < ymin) ymin = yp[i];
                    if (yp[i] > ymax) ymax = yp[i];
                }
                if (ymax - ymin < 8)
                    std::cerr << "WARN: first NV12 frame uniform (Y " << (int)ymin
                              << "-" << (int)ymax << "), GPU 直读 V4L2 缓冲可能不一致"
                              << std::endl;
            }
            auto t_push = std::chrono::steady_clock::now();
            int push_ret = (check_bus(gst_bus) != 0) ? -1 :
                push_frame(appsrc, nv12_frame[pending_slot].data(),
                           output_size, width, height);
            acc_push += ms_since(t_push);
            if (push_ret != 0) {
                std::cerr << "Push frame to encoder pipeline failed" << std::endl;
                break;
            }
        }

        // 等待内核执行完成(kx), 之后采集缓冲已被读完, 归还驱动
        auto t_kx = std::chrono::steady_clock::now();
        clWaitForEvents(1, &kernel_event);
        acc_kx += ms_since(t_kx);
        clReleaseEvent(kernel_event);
        kernel_event = nullptr;
        if (q_buf(cap_fd, cap_buf.index) < 0) {
            std::cerr << "Failed to requeue capture buffer" << std::endl;
            break;
        }

        // 回读本帧 NV12(非阻塞 + event, 下一轮取结果)
        auto t_rd = std::chrono::steady_clock::now();
        err = clEnqueueReadBuffer(queue, out_buffer[slot], CL_FALSE, 0,
                                  output_size, nv12_frame[slot].data(),
                                  0, nullptr, &pending_event);
        acc_rd += ms_since(t_rd);
        CHECK_CL_ERROR(err, "clEnqueueReadBuffer");
        pending_slot = slot;

        frame_count++;
        acc_frames++;
        if (frame_count % 30 == 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed > 0)
                std::cout << "Frames: " << frame_count
                          << ", FPS: " << (frame_count * 1000LL / elapsed) << std::endl;
            // 瓶颈归因: poll=等帧 gpu=上帧回读等待 push=编码链
            //           kx=内核执行 rd=回读DMA入队(驱动同步执行则在此阻塞)
            if (acc_frames > 0)
                std::cout << "  timing(ms/frame): poll " << (acc_poll / acc_frames)
                          << "  gpu " << (acc_gpu / acc_frames)
                          << "  push " << (acc_push / acc_frames)
                          << "  | kx " << (acc_kx / acc_frames)
                          << "  rd " << (acc_rd / acc_frames) << std::endl;
            acc_poll = acc_gpu = acc_push = 0.0;
            acc_kx = acc_rd = 0.0;
            acc_frames = 0;
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
    if (kernel_event) clReleaseEvent(kernel_event);
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
    for (size_t i = 0; i < input_clmem.size(); i++)
        if (input_clmem[i]) clReleaseMemObject(input_clmem[i]);
    if (out_buffer[0]) clReleaseMemObject(out_buffer[0]);
    if (out_buffer[1]) clReleaseMemObject(out_buffer[1]);
    close_capture(cap_fd, cap_bufs);
    return 0;
}
