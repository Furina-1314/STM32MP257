#include "status_printer.h"

#include <cstdio>
#include <unistd.h>

namespace camstream {

StatusPrinter::StatusPrinter()
    : tty_(isatty(STDOUT_FILENO) != 0), status_active_(false) {}

void StatusPrinter::updateStatus(const std::string &l1, const std::string &l2) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_line1_ = l1;
    last_line2_ = l2;
    if (!tty_) {
        // 非 TTY(重定向到文件): 普通逐行追加, 保持日志文件可读
        fprintf(stdout, "%s\n%s\n", l1.c_str(), l2.c_str());
        fflush(stdout);
        return;
    }
    if (status_active_) {
        // 光标在状态块下一行: 上移两行回到 line1, 逐行清行重写
        fputs("\033[2A", stdout);
    }
    fprintf(stdout, "\033[2K%s\n\033[2K%s\n", l1.c_str(), l2.c_str());
    fflush(stdout);
    status_active_ = true;
}

void StatusPrinter::log(const char *prefix, const std::string &msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 日志追加在当前光标处(状态块下方): 状态块保留在其上方不被覆盖;
    // 同时状态块不再处于"光标上方紧邻两行"的位置, 置为非活动, 下次
    // updateStatus 在日志行下方重开新块(等价 clear->log->restore 协议)
    fprintf(stdout, "%s%s\n", prefix, msg.c_str());
    fflush(stdout);
    status_active_ = false;
}

void StatusPrinter::logInfo(const std::string &msg)  { log("[INFO] ", msg); }
void StatusPrinter::logWarn(const std::string &msg)  { log("[WARN] ", msg); }
void StatusPrinter::logError(const std::string &msg) { log("[ERROR] ", msg); }

void StatusPrinter::clearStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tty_ || !status_active_) return;
    // 上移两行, 清除两行, 光标回到状态块原下方位置
    fputs("\033[2A\033[2K\n\033[2K\n", stdout);
    fflush(stdout);
    status_active_ = false;
}

void StatusPrinter::restoreStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tty_ || status_active_) return;
    fprintf(stdout, "%s\n%s\n", last_line1_.c_str(), last_line2_.c_str());
    fflush(stdout);
    status_active_ = true;
}

} // namespace camstream
