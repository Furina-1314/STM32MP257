/*
 * control_console.h - 运行时控制台命令线程(plan §9.2)
 *
 * 独立线程阻塞读 stdin, 不影响采集/处理热路径; 硬件操作全部经
 * CameraControl(互斥锁串行化), 命令解析器不直接碰任何设备或 system().
 *
 * 命令:
 *   help                          命令列表
 *   status                        sensor/ISP 当前状态
 *   sensor gain N                 设 analogue_gain(回读)
 *   sensor exposure N             设 exposure(回读)
 *   sensor vblank N               设 vertical_blanking(下次 STREAMON 生效)
 *   isp profile tl84|d50          切换 ISP profile(重跑 dcmipp-isp-ctrl)
 *   isp contrast none|50|200|dynamic
 *   isp auto-gain run             传感器增益/曝光自动调整
 *   dump frame / dump next N      (Phase 5 提供)
 *   quit                          结束程序(等同 Ctrl-C)
 *
 * 多行输出(help/status/子进程输出)走 StatusPrinter 的
 * clearStatus -> 打印 -> restoreStatus 协议(plan §6.4), 不破坏状态行.
 */

#ifndef CAMSTREAM_CONTROL_CONSOLE_H
#define CAMSTREAM_CONTROL_CONSOLE_H

#include <string>
#include <thread>
#include <csignal>

#include "camera_control.h"
#include "status_printer.h"
#include "app_config.h"

namespace camstream {

class FrameDumper;

class ControlConsole {
public:
    ControlConsole(CameraControl &cam, StatusPrinter &status,
                   volatile sig_atomic_t *stop_flag, FrameDumper *dumper,
                   AppConfig *live_cfg);
    ~ControlConsole();   // join 命令线程(若未 shutdown)

    // 启动 stdin 命令线程(joinable; 由 shutdown() 回收)
    void start();

    // 停止并回收命令线程(阻塞直至线程退出). 依赖 SIGINT 不带 SA_RESTART
    // (见 stream_app 信号设置), 阻塞中的 getline 会因 EINTR 返回而退出;
    // 必须在销毁 status_/cam_/dumper_ 等被引用对象之前调用.
    void shutdown();

private:
    static void threadMain(ControlConsole *self);
    void handleLine(const std::string &line);
    void printHelp();
    void printStatus();
    void handleConfig(const std::string &action, const std::string &file);
    // 多行输出协议: 清状态块 -> 裸打印 -> 恢复状态块
    void printBlock(const std::string &text);

    CameraControl &cam_;
    StatusPrinter &status_;
    volatile sig_atomic_t *stop_flag_;
    FrameDumper *dumper_;   // 可为 nullptr(未启用)
    AppConfig *live_cfg_;   // 可为 nullptr(未启用 save/load config)
    std::thread thread_;
};

} // namespace camstream

#endif // CAMSTREAM_CONTROL_CONSOLE_H
