#include "isp_controller.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace camstream {

IspController::IspController() : cfg_() {}

IspController::IspController(const IspStartupConfig &cfg) : cfg_(cfg) {}

const char *IspController::illuminantName(int t) {
    switch (t) {
    case 0: return "D50";
    case 1: return "TL84";
    default: return "?";
    }
}

const char *IspController::contrastName(int t) {
    switch (t) {
    case 0: return "none";
    case 1: return "50%";
    case 2: return "200%";
    case 3: return "dynamic";
    default: return "unset";
    }
}

// 执行子进程: posix_spawn + argv(无 shell), stdout/stderr 重定向到管道,
// 先读尽管道再 waitpid(若先等待, 子进程可能因管道写满而阻塞死锁).
IspApplyResult IspController::run(const std::vector<std::string> &args) const {
    IspApplyResult res;
    if (access(cfg_.tool_path.c_str(), X_OK) != 0) {
        res.output = "isp tool not found or not executable: " + cfg_.tool_path;
        return res;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        res.output = std::string("pipe() failed: ") + strerror(errno);
        return res;
    }

    // argv 构造 + 命令行展示串
    std::vector<char *> argv;
    std::string cmd = cfg_.tool_path;
    argv.push_back(const_cast<char *>(cfg_.tool_path.c_str()));
    for (size_t i = 0; i < args.size(); i++) {
        argv.push_back(const_cast<char *>(args[i].c_str()));
        cmd += " " + args[i];
    }
    argv.push_back(nullptr);
    res.cmdline = cmd;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);

    pid_t pid = -1;
    int rc = posix_spawn(&pid, cfg_.tool_path.c_str(), &fa, nullptr,
                         argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        res.output = std::string("posix_spawn failed: ") + strerror(rc);
        return res;
    }

    // 读子进程输出直至 EOF(子进程退出后管道写端关闭)
    std::string out;
    char buf[512];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        out.append(buf, (size_t)n);
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    res.ran = true;
    res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    res.output = out;
    return res;
}

IspApplyResult IspController::apply() const {
    std::vector<std::string> args;
    if (cfg_.contrast >= 0) {
        args.push_back("-c");
        args.push_back(std::to_string(cfg_.contrast));
    }
    args.push_back("-i");
    args.push_back(std::to_string(cfg_.illuminant));
    if (cfg_.verbose)
        args.push_back("-v");
    return run(args);
}

IspApplyResult IspController::setIlluminant(int type) {
    if (type != 0 && type != 1) {
        IspApplyResult res;
        res.output = "illuminant must be 0(D50) or 1(TL84)";
        return res;
    }
    cfg_.illuminant = type;
    std::vector<std::string> args;
    args.push_back("-i");
    args.push_back(std::to_string(type));
    if (cfg_.verbose)
        args.push_back("-v");
    return run(args);
}

IspApplyResult IspController::setContrast(int type) {
    if (type < 0 || type > 3) {
        IspApplyResult res;
        res.output = "contrast must be 0(none) 1(50%) 2(200%) 3(dynamic)";
        return res;
    }
    cfg_.contrast = type;
    std::vector<std::string> args;
    args.push_back("-c");
    args.push_back(std::to_string(type));
    if (cfg_.verbose)
        args.push_back("-v");
    return run(args);
}

IspApplyResult IspController::runAutoGain() {
    std::vector<std::string> args;
    args.push_back("-g");
    if (cfg_.verbose)
        args.push_back("-v");
    return run(args);
}

} // namespace camstream
