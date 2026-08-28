/*
 * status_printer.h - 原地刷新的两行状态日志
 *
 * (plan §6) 周期状态固定占两行, 刷新时原地覆盖而不是追加:
 *   - TTY  : ESC[2A 上移两行 + ESC[2K 清行重写
 *   - 非TTY(重定向到文件): 自动回退为普通逐行追加, 文件中无 ANSI 乱码
 * 日志(logInfo/logWarn/logError)始终追加新行: 打印在状态块下方, 之后状态块
 * 在日志行下方重开, 因此 warning/error 不会被状态刷新覆盖.
 *
 * clearStatus()/restoreStatus() 供外部需要直接向 stdout 打印多行输出的
 * 场景(如 Phase 2 的 dcmipp-isp-ctrl -v 输出): 先 clear, 输出完再 restore.
 *
 * 所有接口线程安全(内部互斥), 供 Phase 4 控制线程与主循环并发调用.
 */

#ifndef CAMSTREAM_STATUS_PRINTER_H
#define CAMSTREAM_STATUS_PRINTER_H

#include <string>
#include <mutex>

namespace camstream {

class StatusPrinter {
public:
    StatusPrinter();   // 构造时检测 stdout 是否 TTY

    // 覆盖式刷新两行状态(非 TTY 退化为追加两行)
    void updateStatus(const std::string &line1, const std::string &line2);

    // 追加一行日志(带前缀, 永不覆盖)
    void logInfo(const std::string &msg);
    void logWarn(const std::string &msg);
    void logError(const std::string &msg);

    // 清除当前状态块(外部直接输出前调用); 状态行内容被记住
    void clearStatus();
    // 在当前位置重印状态块(外部输出完成后调用)
    void restoreStatus();

    bool isTty() const { return tty_; }

private:
    StatusPrinter(const StatusPrinter &);
    StatusPrinter &operator=(const StatusPrinter &);

    void log(const char *prefix, const std::string &msg);

    bool tty_;
    bool status_active_;          // 状态块当前是否在屏上
    std::string last_line1_, last_line2_;
    std::mutex mutex_;            // 主循环/控制线程并发保护
};

} // namespace camstream

#endif // CAMSTREAM_STATUS_PRINTER_H
