/*
 * app_config.h - 运行配置结构与命令行解析
 *
 * 原先两个 main 中的默认值宏(设备/IP/端口/码率/帧率/分辨率/缓冲数/SDP名)
 * 收敛为一份 AppConfig 结构, 由两档工厂函数给出各自默认值:
 *   - defaultConfig1080p(): 1080p@12fps (稳定工作点)
 *   - defaultConfig720p() : 720p@30fps
 *
 * 位置参数与原实现完全兼容:
 *   ./rgb_xxx [采集设备] [目标IP] [端口] [码率kbps] [帧率]
 * 后续 Phase 的 --xxx 命名参数在此解析函数上扩展, 不破坏位置参数.
 */

#ifndef CAMSTREAM_APP_CONFIG_H
#define CAMSTREAM_APP_CONFIG_H

#include <string>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "isp_controller.h"   // IspStartupConfig

namespace camstream {

struct AppConfig {
    // ---- 采集/链路 ----
    std::string device;          // DCMIPP main postproc 输出节点
    uint32_t    sensor_width;    // 传感器->CSI->ISP 链路分辨率
    uint32_t    sensor_height;
    uint32_t    out_width;       // postproc 输出 = 采集/GPU/编码分辨率(须为偶数)
    uint32_t    out_height;
    int         cap_buf_count;   // V4L2 采集缓冲区个数

    // ---- 推流 ----
    std::string dest_ip;         // 接收主机IP
    int         dest_port;
    int         bitrate_kbps;    // H264 目标码率
    int         gop = 0;         // --gop N: keyframe-interval; 0=默认(fps<10?10:fps)
    int         colorimetry = 0; // --colorimetry: 0=bt601(默认) 1=bt709
    int         appsrc_queue = 1; // --appsrc-queue N: appsrc 积压帧数上限(默认1, 低延迟)

    // ---- 帧率 ----
    int         target_fps;      // 0 = 不节拍(传感器模式默认); >0 = 节拍目标
    int         fallback_fps;    // 探测失败时的兜底帧率

    // ---- 传感器曝光控制(无 libcamera 3A, 黑屏与否的关键) ----
    int         sensor_gain;     // analogue_gain(0x00980917 类控件, 按名称解析)
    int         sensor_exposure; // exposure

    // ---- 诊断 ----
    uint64_t    dump_frame;      // --dump-frame N: 只落盘第 N 帧(0=off)
    uint64_t    dump_every;      // --dump-every N: 每 N 帧一帧(0=off)
    uint64_t    dump_count;      // --dump-count M: every 模式上限(0=不限)
    bool        input_copy = false; // --input-mode copy: 显式整帧拷贝诊断模式
                                    // (默认 false = hostptr 零拷贝性能基准)

    // ---- ISP ----
    IspStartupConfig isp;        // 启动时自动应用的 ISP profile(默认 TL84)

    // ---- 辅助 ----
    std::string sdp_file;        // 启动时生成的接收端 SDP 文件名

    // 启动摘要(plan §9.1: 所有参数启动时打印最终值)
    std::string toString() const {
        char buf[640];
        snprintf(buf, sizeof(buf),
                 "device=%s sensor=%ux%u out=%ux%u dest=%s:%d bitrate=%dkbps "
                 "fps=%d bufs=%d sdp=%s | sensor: gain=%d exposure=%d\n"
                 "isp: enabled=%d illuminant=%s contrast=%s required=%d",
                 device.c_str(), sensor_width, sensor_height,
                 out_width, out_height, dest_ip.c_str(), dest_port,
                 bitrate_kbps, target_fps, cap_buf_count, sdp_file.c_str(),
                 sensor_gain, sensor_exposure,
                 isp.enabled ? 1 : 0, IspController::illuminantName(isp.illuminant),
                 IspController::contrastName(isp.contrast), isp.required ? 1 : 0);
        return std::string(buf);
    }
};

inline AppConfig defaultConfig1080p() {
    AppConfig c;
    c.device        = "/dev/video1";   // DCMIPP main postproc 输出节点
    c.sensor_width  = 1920;            // 传感器->CSI->ISP 链路分辨率
    c.sensor_height = 1080;
    c.out_width     = 1920;            // 1080p 模式: 输出 = 传感器全幅
    c.out_height    = 1080;
    c.cap_buf_count = 4;
    c.dest_ip       = "192.168.1.100"; // 接收主机IP
    c.dest_port     = 5000;
    c.bitrate_kbps  = 4000;
    c.target_fps    = 0;               // 0=不降帧(传感器模式默认30fps, 处理链限流~24fps)
    c.fallback_fps  = 30;              // 探测失败兜底 = 模式默认
    c.sensor_gain   = 35;              // 原硬编码默认值(已实机验证不过曝/不黑屏)
    c.sensor_exposure = 2000;
    c.dump_frame    = 0;
    c.dump_every    = 0;
    c.dump_count    = 0;
    c.sdp_file      = "stream_1080p.sdp";
    return c;
}

inline AppConfig defaultConfig720p() {
    AppConfig c;
    c.device        = "/dev/video1";
    c.sensor_width  = 1920;            // 传感器仍按全幅出流
    c.sensor_height = 1080;
    c.out_width     = 1280;            // postproc 硬件缩放输出 720p
    c.out_height    = 720;
    c.cap_buf_count = 4;
    c.dest_ip       = "192.168.1.100";
    c.dest_port     = 5000;
    c.bitrate_kbps  = 3000;
    c.target_fps    = 0;               // 0=不降帧(传感器模式默认30fps, 与720p目标一致)
    c.fallback_fps  = 30;              // 探测失败兜底 = 模式默认
    c.sensor_gain   = 35;
    c.sensor_exposure = 2000;
    c.dump_frame    = 0;
    c.dump_every    = 0;
    c.dump_count    = 0;
    c.sdp_file      = "stream_720p.sdp";
    return c;
}

// 参数解析: 前 5 个位置参数与原 main 语义一致:
//   [采集设备] [目标IP] [端口] [码率kbps] [帧率(0=不降帧)]
// 命名参数(可出现在任意位置, 逐步扩展):
//   --sensor-gain N               传感器 analogue_gain(默认 35)
//   --sensor-exposure N           传感器 exposure(默认 2000)
//   --isp-profile tl84|d50|none   启动 ISP profile(默认 tl84)
//   --isp-contrast none|50|200|dynamic (或 0..3)
//   --require-isp                 ISP 应用失败则启动失败(默认仅告警)
//   --dump-frame N / --dump-every N / --dump-count M   NV12 落盘
//   --input-mode hostptr|copy     GPU 输入模式(默认 hostptr; copy 为诊断 A/B)
//   --gop N                       keyframe-interval(默认 fps<10?10:fps)
//   --colorimetry bt601|bt709     色彩空间(默认 bt601; 内核矩阵同步切换)
// 未知命名参数打印告警并忽略. 返回解析后的配置副本.
inline AppConfig parseArgs(int argc, char *argv[], const AppConfig &def) {
    AppConfig cfg = def;
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--sensor-gain") {
            if (i + 1 >= argc || atoi(argv[i + 1]) <= 0) {
                fprintf(stderr, "WARN: --sensor-gain needs a positive value\n");
                if (i + 1 < argc) i++;
                continue;
            }
            cfg.sensor_gain = atoi(argv[++i]);
        } else if (a == "--sensor-exposure") {
            if (i + 1 >= argc || atoi(argv[i + 1]) <= 0) {
                fprintf(stderr, "WARN: --sensor-exposure needs a positive value\n");
                if (i + 1 < argc) i++;
                continue;
            }
            cfg.sensor_exposure = atoi(argv[++i]);
        } else if (a == "--isp-profile") {
            if (i + 1 >= argc) { fprintf(stderr, "WARN: --isp-profile needs a value (tl84|d50|none)\n"); continue; }
            std::string v = argv[++i];
            if (v == "tl84")      { cfg.isp.enabled = true; cfg.isp.illuminant = 1; }
            else if (v == "d50")  { cfg.isp.enabled = true; cfg.isp.illuminant = 0; }
            else if (v == "none") { cfg.isp.enabled = false; }
            else fprintf(stderr, "WARN: unknown --isp-profile '%s' (tl84|d50|none)\n", v.c_str());
        } else if (a == "--isp-contrast") {
            if (i + 1 >= argc) { fprintf(stderr, "WARN: --isp-contrast needs a value (none|50|200|dynamic)\n"); continue; }
            std::string v = argv[++i];
            int c = -1;
            if (v == "none") c = 0;
            else if (v == "50") c = 1;
            else if (v == "200") c = 2;
            else if (v == "dynamic") c = 3;
            else if (v.size() == 1 && v[0] >= '0' && v[0] <= '3') c = v[0] - '0';
            if (c >= 0) cfg.isp.contrast = c;
            else fprintf(stderr, "WARN: unknown --isp-contrast '%s' (none|50|200|dynamic)\n", v.c_str());
        } else if (a == "--require-isp") {
            cfg.isp.required = true;
        } else if (a == "--dump-frame") {
            if (i + 1 >= argc || strtoull(argv[i + 1], nullptr, 10) == 0) {
                fprintf(stderr, "WARN: --dump-frame needs a positive frame number\n");
                if (i + 1 < argc) i++;
                continue;
            }
            cfg.dump_frame = strtoull(argv[++i], nullptr, 10);
        } else if (a == "--dump-every") {
            if (i + 1 >= argc || strtoull(argv[i + 1], nullptr, 10) == 0) {
                fprintf(stderr, "WARN: --dump-every needs a positive interval\n");
                if (i + 1 < argc) i++;
                continue;
            }
            cfg.dump_every = strtoull(argv[++i], nullptr, 10);
        } else if (a == "--dump-count") {
            if (i + 1 >= argc) {
                fprintf(stderr, "WARN: --dump-count needs a value\n");
                continue;
            }
            cfg.dump_count = strtoull(argv[++i], nullptr, 10);
        } else if (a == "--gop") {
            if (i + 1 >= argc || atoi(argv[i + 1]) <= 0) {
                fprintf(stderr, "WARN: --gop needs a positive value\n");
                if (i + 1 < argc) i++;
                continue;
            }
            cfg.gop = atoi(argv[++i]);
        } else if (a == "--appsrc-queue") {
            if (i + 1 >= argc || atoi(argv[i + 1]) < 1) {
                fprintf(stderr, "WARN: --appsrc-queue needs a value >= 1 (frames)\n");
                if (i + 1 < argc) i++;
                continue;
            }
            cfg.appsrc_queue = atoi(argv[++i]);
        } else if (a == "--colorimetry") {
            if (i + 1 >= argc) { fprintf(stderr, "WARN: --colorimetry needs a value (bt601|bt709)\n"); continue; }
            std::string v = argv[++i];
            if (v == "bt601")      cfg.colorimetry = 0;
            else if (v == "bt709") cfg.colorimetry = 1;
            else fprintf(stderr, "WARN: unknown --colorimetry '%s' (bt601|bt709)\n", v.c_str());
        } else if (a == "--input-mode") {
            if (i + 1 >= argc) { fprintf(stderr, "WARN: --input-mode needs a value (hostptr|copy)\n"); continue; }
            std::string v = argv[++i];
            if (v == "hostptr")     cfg.input_copy = false;
            else if (v == "copy")   cfg.input_copy = true;
            else fprintf(stderr, "WARN: unknown --input-mode '%s' (hostptr|copy)\n", v.c_str());
        } else if (a.compare(0, 2, "--") == 0) {
            fprintf(stderr, "WARN: unknown option '%s' ignored\n", a.c_str());
        } else {
            switch (positional++) {
            case 0: if (!a.empty()) cfg.device = a; break;
            case 1: if (!a.empty()) cfg.dest_ip = a; break;
            case 2: if (atoi(a.c_str()) > 0)  cfg.dest_port = atoi(a.c_str()); break;
            case 3: if (atoi(a.c_str()) > 0)  cfg.bitrate_kbps = atoi(a.c_str()); break;
            case 4: if (atoi(a.c_str()) >= 0) cfg.target_fps = atoi(a.c_str()); break;
            default: fprintf(stderr, "WARN: extra positional arg '%s' ignored\n", a.c_str()); break;
            }
        }
    }
    return cfg;
}

} // namespace camstream

#endif // CAMSTREAM_APP_CONFIG_H
