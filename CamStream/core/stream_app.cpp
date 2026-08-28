#include "stream_app.h"

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <csignal>
#include <signal.h>
#include <cstring>
#include <cstdio>
#include <cstdint>

#include "dcmipp_setup.h"
#include "v4l2_capture.h"
#include "cl_converter.h"
#include "gst_streamer.h"
#include "isp_controller.h"
#include "status_printer.h"
#include "camera_control.h"
#include "control_console.h"
#include "frame_dumper.h"

namespace camstream {

// ====== 错误检查宏(与原 main 相同的 goto cleanup 模式; 所有局部变量
//         先于任何 goto 声明, RAII 模块析构完成资源释放) ======
#define CHECK_CL(err, msg) \
    if ((err) != CL_SUCCESS) { \
        std::cerr << "OpenCL Error [" << msg << "]: " << err << std::endl; \
        goto cleanup; \
    }

// ====== 全局运行开关 =====
static volatile sig_atomic_t g_stop = 0;
static void handle_signal(int) { g_stop = 1; }

// 距 t 时刻的毫秒数(主循环 timing 仪表用)
static double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t).count();
}

int runStreamApp(const AppConfig &cfg_def) {
    // 运行期可变副本: load config 命令实时更新 sensor/isp 项并把
    // 重启生效项写回(下次启动由 main 以文件/参数重新构造配置)
    AppConfig cfg = cfg_def;
    // ------ 所有作用域变量先于任何 goto 声明并初始化 ------
    const char *dev = cfg.device.c_str();
    const char *dest_ip = cfg.dest_ip.c_str();
    const int dest_port = cfg.dest_port;
    const int bitrate_kbps = cfg.bitrate_kbps;
    const int target_fps = cfg.target_fps;

    const uint32_t sensor_width = cfg.sensor_width, sensor_height = cfg.sensor_height;
    const uint32_t width = cfg.out_width, height = cfg.out_height;   // 输出(采集/GPU/编码)尺寸
    uint32_t in_stride = width * 3;                          // open_capture 后用驱动报告值
    const size_t input_size = (size_t)width * height * 3;
    size_t input_span = input_size;   // 内核实际访问跨度: (H-1)*stride + W*3
    const size_t output_size = (size_t)width * height * 3 / 2;

    ClConverter conv;                 // 析构释放全部 OpenCL 资源
    GstStreamer gst;                  // 析构: STATE_NULL + unref
    // 运行时控制对象: 依赖 sensor_node(运行时解析), 只能延迟构造;
    // 为满足"先于任何 goto 声明初始化"的 goto 约束, 以指针持有
    CameraControl *cam_ctl = nullptr;
    ControlConsole *console = nullptr;
    FrameDumper dumper;      // 按需 NV12 落盘(--dump-* / 运行时 dump 命令)

    cl_int err = CL_SUCCESS;
    cl_event pending_event = nullptr;
    cl_event kernel_event = nullptr;
    int pending_slot = -1;
    struct v4l2_buffer cap_buf;
    int cap_fd = -1;
    int slot = 0;
    bool dumped = false;
    int r = 0;
    int stream_fps = 0;
    double measured_fps = 0.0;
    double acc_poll = 0.0, acc_gpu = 0.0, acc_push = 0.0;
    double acc_push_max = 0.0;      // 区间内 push 峰值(编码链瞬时阻塞指标)
    double acc_kx = 0.0, acc_rd = 0.0;
    int acc_frames = 0;
    uint32_t frame_count = 0;
    uint32_t drop_count = 0;        // 区间内 dq 空转计数(poll 超时/EAGAIN)
    uint32_t total_drop = 0;        // 累计 dq 空转计数
    uint32_t last_stat_frames = 0;
    std::vector<CaptureBuffer> cap_bufs;
    std::vector<uint8_t> nv12_frame[2] = {
        std::vector<uint8_t>(output_size),
        std::vector<uint8_t>(output_size)
    };
    std::string sensor_node;
    auto last_stat_time = std::chrono::steady_clock::now();
    StatusPrinter status;           // 两行原地刷新状态(plan §6)

    if (width % 2 != 0 || height % 2 != 0) {
        std::cerr << "Width/height must be even (2x2 block kernel)" << std::endl;
        return 1;
    }

    // SIGINT/SIGTERM 刻意不带 SA_RESTART: glibc signal() 默认自动重启被
    // 信号打断的阻塞 read, 导致控制台线程在 Ctrl-C 后仍阻塞于
    // std::getline(std::cin), 进程退出时 std::cin 静态析构与该线程竞争
    // (退出期 Segmentation fault 根因). 不重启则 getline 因 EINTR 返回,
    // 命令线程退出, cleanup 中 console->shutdown() 可靠 join.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sa.sa_flags = 0;   // 不设 SA_RESTART
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // ------ 1. 配置 DCMIPP media 管线 (重启后会复位, 必须先于采集) ------
    if (!setup_dcmipp_pipeline(sensor_width, sensor_height, width, height,
                               cfg.sensor_gain, cfg.sensor_exposure,
                               sensor_node)) {
        std::cerr << "Failed to configure DCMIPP media pipeline" << std::endl;
        return 1;
    }

    // ------ 1.5 传感器时序: 必须先于 STREAMON(IMX335 驱动只在启动流时应用
    //             vblank, 流状态下写入不生效; 且 vblank 跨运行持久, 需清除
    //             旧运行残留的节拍值) ------
    // target_fps=0(默认, 不降帧): 恢复 vblank 为驱动默认值(模式默认 30fps);
    // target_fps>0              : 按板级校准模型节拍到指定帧率.
    prepare_sensor_timing(sensor_node, target_fps);

    // ------ 2. V4L2 采集初始化 ------
    cap_fd = open_capture(dev, width, height, cfg.cap_buf_count, cap_bufs, in_stride);
    if (cap_fd < 0) {
        std::cerr << "Failed to setup V4L2 capture" << std::endl;
        return 1;
    }
    input_span = (size_t)(height - 1) * in_stride + (size_t)width * 3;

    // ------ 2.5 ISP profile 自动应用(plan §7): 必须在 STREAMON 成功之后
    //             (ST 要求 camera pipeline 正在运行), 先于 fps 探测;
    //             已实机验证 dcmipp-isp-ctrl -i 1 -v 可恢复全局偏色 ------
    //             工具输出打印在状态区启动之前, 不破坏 ANSI 光标(plan §6.4)
    if (cfg.isp.enabled) {
        IspController isp(cfg.isp);
        std::cout << "[ISP] startup    : enabled (illuminant "
                  << IspController::illuminantName(cfg.isp.illuminant);
        if (cfg.isp.contrast >= 0)
            std::cout << ", contrast "
                      << IspController::contrastName(cfg.isp.contrast);
        std::cout << ")" << std::endl;
        IspApplyResult r = isp.apply();
        if (!r.output.empty())
            std::cout << "[ISP] tool output:" << std::endl << r.output << std::endl;
        if (r.ran && r.exit_code == 0) {
            std::cout << "[ISP] apply      : OK (exit 0)" << std::endl;
        } else {
            std::cerr << "[WARN] ISP control command failed"
                      << (r.ran ? ", exit=" + std::to_string(r.exit_code) : "")
                      << " : " << (r.ran ? r.cmdline : r.output) << std::endl;
            if (cfg.isp.required) {
                std::cerr << "[ERROR] --require-isp set, aborting startup"
                          << std::endl;
                goto cleanup;
            }
        }
    } else {
        std::cout << "[ISP] startup    : disabled (--isp-profile none)"
                  << std::endl;
    }

    // ------ 3. 帧率: 时序已在 STREAMON 前设置(见 1.5 步), 这里只探测实测值
    //            作流元数据; 流状态下写 vblank 不生效, 故不再重试 ------
    measured_fps = probe_capture_fps(cap_fd);
    if (target_fps > 0 && measured_fps > 0.0 && measured_fps < 0.85 * target_fps)
        std::cerr << "WARN: sensor delivers " << measured_fps << "fps, target "
                  << target_fps << "fps not reached (vblank calibration off)"
                  << std::endl;
    if (measured_fps <= 0.0)
        measured_fps = (target_fps > 0) ? target_fps : cfg.fallback_fps;
    stream_fps = (int)(measured_fps + 0.5);
    if (stream_fps < 1) stream_fps = 1;
    std::cout << "Stream fps: " << stream_fps
              << " (measured " << measured_fps << ")" << std::endl;

    // ------ 4. OpenCL 初始化 ------
    std::cout << "[GPU] input mode  : "
              << (cfg.input_copy ? "copy (diagnostic)" : "hostptr") << std::endl;
    std::cout << "[YUV] colorimetry : "
              << (cfg.colorimetry == 1 ? "bt709 limited" : "bt601 limited")
              << std::endl;
    err = conv.init(width, height, in_stride, input_span, output_size,
                    cap_bufs, "rgb_to_nv12.cl",
                    cfg.input_copy ? INPUT_COPY : INPUT_HOST_PTR,
                    cfg.colorimetry == 1 ? "-DUSE_BT709" : "");
    CHECK_CL(err, "ClConverter::init");

    // ------ 5. 启动硬件编码推流管线 ------
    if (cfg.gop > 0)
        std::cout << "[ENC] GOP         : " << cfg.gop << " (explicit)" << std::endl;
    if (cfg.appsrc_queue != 1)
        std::cout << "[ENC] appsrc queue: " << cfg.appsrc_queue
                  << " frames (A/B experiment; default 1 = lowest latency)"
                  << std::endl;
    if (!gst.build(dest_ip, dest_port, width, height,
                   bitrate_kbps, stream_fps, cfg.sdp_file.c_str(),
                   cfg.gop, cfg.colorimetry, cfg.appsrc_queue))
        goto cleanup;

    // ------ 6. 主循环: 采集 -> GPU 转换(与推送重叠) -> 回读 ------
    // 运行时控制台(plan §9.2): 独立 stdin 线程, 硬件操作经 CameraControl
    // 互斥串行化, 不触碰采集热路径
    cam_ctl = new CameraControl(sensor_node, cfg.isp);
    console = new ControlConsole(*cam_ctl, status, &g_stop, &dumper, &cfg);
    console->start();
    if (cfg.dump_frame > 0)
        dumper.configureFrame(cfg.dump_frame);
    if (cfg.dump_every > 0)
        dumper.configureEvery(cfg.dump_every, cfg.dump_count);
    std::cout << "Console: type 'help' + Enter for runtime commands" << std::endl;
    if (cfg.dump_frame > 0)
        std::cout << "[DUMP] will save frame " << cfg.dump_frame << std::endl;
    if (cfg.dump_every > 0)
        std::cout << "[DUMP] every " << cfg.dump_every << " frame(s)"
                  << (cfg.dump_count > 0
                          ? ", up to " + std::to_string(cfg.dump_count)
                          : std::string(", unlimited"))
                  << std::endl;

    std::cout << "Streaming " << dev << " -> " << dest_ip << ":" << dest_port
              << " (Ctrl-C to stop)" << std::endl;
    last_stat_time = std::chrono::steady_clock::now();
    last_stat_frames = 0;

    while (!g_stop) {
        auto t_poll = std::chrono::steady_clock::now();
        r = dq_frame(cap_fd, cap_buf);
        acc_poll += ms_since(t_poll);
        if (r < 0) {
            status.logError("Capture error, exiting");
            break;
        }
        if (r == 0) { drop_count++; total_drop++; continue; }   // 无帧, 回到 poll 休眠

        // 提交本帧 GPU 转换(异步): 先入队, 让 GPU 与下面的推送工作重叠.
        // 采集缓冲在内核读完(kx 等待返回)之前不归还给驱动.
        slot = (int)(frame_count & 1);
        err = conv.enqueueConvert(cap_bufs[cap_buf.index].start, cap_buf.index,
                                  slot, &kernel_event);
        CHECK_CL(err, "clEnqueueNDRangeKernel");

        // 上一帧: 等回读完成, 送入编码管线(GPU 同时在算本帧)
        if (pending_event) {
            auto t_gpu = std::chrono::steady_clock::now();
            err = clWaitForEvents(1, &pending_event);
            acc_gpu += ms_since(t_gpu);
            clReleaseEvent(pending_event);
            pending_event = nullptr;
            if (err != CL_SUCCESS) {
                status.logError("clWaitForEvents error: " + std::to_string(err));
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
                    status.logWarn("first NV12 frame uniform (Y " +
                        std::to_string((int)ymin) + "-" + std::to_string((int)ymax) +
                        "), GPU 直读 V4L2 缓冲可能不一致");
            }
            // 按需 NV12 落盘(此时 pending 帧已回读完成, frame_count 恰为
            // 该帧的 1 基帧号): OpenCL 输出, 位于编码之前, 用作
            // "前端转换 vs 后端编码/网络"的分界诊断
            if (dumper.shouldDump(frame_count)) {
                std::string dpath;
                if (dumper.dump(frame_count, nv12_frame[pending_slot].data(),
                                output_size, dpath))
                    status.logInfo("[DUMP] " + dpath);
                else
                    status.logWarn("[DUMP] frame " + std::to_string(frame_count) +
                                   " failed");
            }
            auto t_push = std::chrono::steady_clock::now();
            int push_ret = (gst.checkBus() != 0) ? -1 :
                gst.pushFrame(nv12_frame[pending_slot].data(), output_size,
                              width, height);
            {
                double pv = ms_since(t_push);
                acc_push += pv;
                if (pv > acc_push_max) acc_push_max = pv;
            }
            if (push_ret != 0) {
                status.logError("Push frame to encoder pipeline failed");
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
            status.logError("Failed to requeue capture buffer");
            break;
        }

        // 回读本帧 NV12(非阻塞 + event, 下一轮取结果)
        auto t_rd = std::chrono::steady_clock::now();
        err = conv.enqueueReadback(slot, nv12_frame[slot].data(), output_size,
                                   &pending_event);
        acc_rd += ms_since(t_rd);
        CHECK_CL(err, "clEnqueueReadBuffer");
        pending_slot = slot;

        frame_count++;
        acc_frames++;
        if (frame_count % 30 == 0) {
            auto now = std::chrono::steady_clock::now();
            long long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_stat_time).count();
            if (elapsed_ms > 0 && acc_frames > 0) {
                // 输出 FPS = 区间内送入编码管线的帧率;
                // drop = 区间 dq 空转, tot = 累计; q = appsrc 积压(帧);
                // push(max) = 编码链推送峰值(瞬时阻塞指标)
                double q_frames = (double)gst.queuedBytes() / (double)output_size;
                char l1[176], l2[208];
                snprintf(l1, sizeof(l1),
                         "Frames: %u | FPS: %.1f | drop: %u | tot: %u | q: %.2ff",
                         frame_count,
                         (frame_count - last_stat_frames) * 1000.0 / (double)elapsed_ms,
                         drop_count, total_drop, q_frames);
                // 瓶颈归因: poll=等帧 gpu=上帧回读等待 push=编码链
                //           kx=内核执行 rd=回读DMA入队(驱动同步执行则在此阻塞)
                snprintf(l2, sizeof(l2),
                         "Timing: poll %.2f | gpu %.2f | push %.2f(max %.1f)"
                         " | kx %.2f | rd %.2f ms",
                         acc_poll / acc_frames, acc_gpu / acc_frames,
                         acc_push / acc_frames, acc_push_max,
                         acc_kx / acc_frames, acc_rd / acc_frames);
                status.updateStatus(l1, l2);
            }
            acc_poll = acc_gpu = acc_push = 0.0;
            acc_push_max = 0.0;
            acc_kx = acc_rd = 0.0;
            acc_frames = 0;
            drop_count = 0;
            last_stat_frames = frame_count;
            last_stat_time = std::chrono::steady_clock::now();
        }
    }

    // 冲刷流水线中最后一帧
    if (pending_event) {
        clWaitForEvents(1, &pending_event);
        clReleaseEvent(pending_event);
        pending_event = nullptr;
        gst.pushFrame(nv12_frame[pending_slot].data(), output_size, width, height);
    }

cleanup:
    status.clearStatus();   // 退出前清掉状态块, 让收尾信息正常显示
    std::cout << "Cleaning up, total frames: " << frame_count << std::endl;
    if (pending_event) clReleaseEvent(pending_event);
    if (kernel_event) clReleaseEvent(kernel_event);
    // 先回收命令线程再销毁其引用的对象(status/cam/dumper);
    // SIGINT 无 SA_RESTART, 阻塞中的 getline 已因 EINTR 退出
    if (console) console->shutdown();
    delete console;
    delete cam_ctl;
    // 释放顺序(与原 main 一致): 先编码管线, 再 OpenCL, 最后关闭采集.
    // 关键: USE_HOST_PTR 输入 cl_mem 对 V4L2 mmap 缓冲持有 GPU 侧映射,
    // 必须先 conv.destroy() 解除, 再 close_capture 释放 CMA 页, 否则内核
    // 报 "N pages are still in use"(每缓冲一次, CMA 随运行泄漏)
    gst.destroy();
    conv.destroy();
    close_capture(cap_fd, cap_bufs);
    return 0;
}

} // namespace camstream
