#include "control_console.h"

#include <iostream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cctype>

#include "frame_dumper.h"
#include "camera_config.h"

namespace camstream {

static std::string lower(std::string s) {
    for (size_t i = 0; i < s.size(); i++)
        s[i] = (char)tolower((unsigned char)s[i]);
    return s;
}

ControlConsole::ControlConsole(CameraControl &cam, StatusPrinter &status,
                               volatile sig_atomic_t *stop_flag,
                               FrameDumper *dumper, AppConfig *live_cfg)
    : cam_(cam), status_(status), stop_flag_(stop_flag), dumper_(dumper),
      live_cfg_(live_cfg) {}

ControlConsole::~ControlConsole() {
    shutdown();
}

void ControlConsole::start() {
    thread_ = std::thread(threadMain, this);
}

void ControlConsole::shutdown() {
    if (thread_.joinable())
        thread_.join();
}

void ControlConsole::threadMain(ControlConsole *self) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (*self->stop_flag_)
            break;
        self->handleLine(line);
        if (*self->stop_flag_)
            break;
    }
}

// 多行输出协议(plan §6.4): 清状态块 -> 裸打印 -> 恢复状态块
void ControlConsole::printBlock(const std::string &text) {
    status_.clearStatus();
    fputs(text.c_str(), stdout);
    fflush(stdout);
    status_.restoreStatus();
}

void ControlConsole::printHelp() {
    printBlock(
        "---- runtime commands ----\n"
        "  help | status | quit\n"
        "  sensor gain N | sensor exposure N | sensor vblank N\n"
        "  isp profile tl84|d50\n"
        "  isp contrast none|50|200|dynamic\n"
        "  isp auto-gain run\n"
        "  dump frame | dump next N        (debug/frame_XXXXXX.nv12)\n"
        "  save config FILE | load config FILE\n"
        "note: sensor vblank takes effect at next STREAMON\n"
        "note: load config applies sensor/isp live; bitrate/gop/colorimetry\n"
        "      /input_mode/appsrc_queue/fps take effect after restart\n");
}

void ControlConsole::printStatus() {
    std::ostringstream oss;
    oss << "---- camera status ----\n";
    oss << "sensor node: " << (cam_.sensorReady() ? cam_.sensorNode() : "(unknown)")
        << "\n";
    int64_t v = 0;
    std::string log;
    if (cam_.getSensor("analogue_gain", v, log))
        oss << "  gain      = " << (long long)v << "\n";
    if (cam_.getSensor("exposure", v, log))
        oss << "  exposure  = " << (long long)v << "\n";
    if (cam_.getSensor("vertical_blanking", v, log))
        oss << "  vblank    = " << (long long)v << "\n";
    const IspStartupConfig &ic = cam_.ispConfig();
    oss << "isp profile = " << IspController::illuminantName(ic.illuminant)
        << ", contrast = " << IspController::contrastName(ic.contrast) << "\n";
    printBlock(oss.str());
}

// save config FILE / load config FILE(plan §15)
// load: sensor/isp 项实时应用; 编码链项(bitrate/gop/colorimetry/
//       input_mode/appsrc_queue/fps)仅更新内存配置并提示重启生效
void ControlConsole::handleConfig(const std::string &action,
                                  const std::string &file) {
    if (!live_cfg_) {
        status_.logWarn("[CTRL] config commands unavailable");
        return;
    }
    if (file.empty()) {
        status_.logWarn("[CTRL] usage: " + action + " config <file>");
        return;
    }
    if (action == "save") {
        if (CameraConfig::save(file, *live_cfg_))
            status_.logInfo("[CTRL] config saved to " + file);
        else
            status_.logWarn("[CTRL] config save failed: " + file);
        return;
    }
    // load
    AppConfig loaded = *live_cfg_;
    if (!CameraConfig::load(file, loaded)) {
        status_.logWarn("[CTRL] config load failed: " + file);
        return;
    }
    std::string applied, restart;
    std::string log;
    // ---- 实时项: sensor ----
    if (loaded.sensor_gain != live_cfg_->sensor_gain) {
        if (cam_.setSensorGain(loaded.sensor_gain, log))
            applied += "sensor_gain ";
        status_.logInfo(log);
    }
    if (loaded.sensor_exposure != live_cfg_->sensor_exposure) {
        if (cam_.setSensorExposure(loaded.sensor_exposure, log))
            applied += "sensor_exposure ";
        status_.logInfo(log);
    }
    // ---- 实时项: isp ----
    if (loaded.isp.enabled != live_cfg_->isp.enabled ||
        loaded.isp.illuminant != live_cfg_->isp.illuminant) {
        if (!loaded.isp.enabled) {
            applied += "isp_profile=none(no-op) ";
        } else if (cam_.setIlluminant(loaded.isp.illuminant, log)) {
            applied += "isp_profile ";
            status_.logInfo(log);
            if (!cam_.lastIspOutput().empty())
                printBlock(cam_.lastIspOutput());
        } else {
            status_.logWarn(log);
        }
    }
    if (loaded.isp.contrast != live_cfg_->isp.contrast) {
        if (loaded.isp.contrast < 0) {
            applied += "isp_contrast=unset(no-op) ";
        } else if (cam_.setContrast(loaded.isp.contrast, log)) {
            applied += "isp_contrast ";
            status_.logInfo(log);
        } else {
            status_.logWarn(log);
        }
    }
    // ---- 重启项 ----
    if (loaded.bitrate_kbps != live_cfg_->bitrate_kbps) restart += "bitrate ";
    if (loaded.gop != live_cfg_->gop) restart += "gop ";
    if (loaded.colorimetry != live_cfg_->colorimetry) restart += "colorimetry ";
    if (loaded.input_copy != live_cfg_->input_copy) restart += "input_mode ";
    if (loaded.appsrc_queue != live_cfg_->appsrc_queue) restart += "appsrc_queue ";
    if (loaded.target_fps != live_cfg_->target_fps) restart += "fps ";
    *live_cfg_ = loaded;
    std::string msg = "[CTRL] config loaded from " + file;
    if (!applied.empty()) msg += " | live: " + applied;
    if (!restart.empty()) msg += " | restart required: " + restart;
    status_.logInfo(msg);
}

void ControlConsole::handleLine(const std::string &line) {
    std::istringstream iss(line);
    std::string a, b, c;
    if (!(iss >> a))
        return;                       // 空行
    iss >> b >> c;
    a = lower(a);

    if (a == "help" || a == "?") {
        printHelp();
    } else if (a == "status") {
        printStatus();
    } else if (a == "quit" || a == "exit") {
        status_.logInfo("[CTRL] quit requested, stopping...");
        *stop_flag_ = 1;
    } else if (a == "sensor") {
        std::string what = lower(b);
        std::string num = c;
        if (what != "gain" && what != "exposure" && what != "vblank") {
            status_.logWarn("[CTRL] usage: sensor gain|exposure|vblank N");
            return;
        }
        char *end = nullptr;
        long long n = strtoll(num.c_str(), &end, 10);
        if (num.empty() || !end || *end != '\0') {
            status_.logWarn("[CTRL] sensor " + what + ": need a number");
            return;
        }
        std::string log;
        bool ok = (what == "gain")     ? cam_.setSensorGain(n, log) :
                  (what == "exposure") ? cam_.setSensorExposure(n, log) :
                                         cam_.setSensorVBlank(n, log);
        if (ok) {
            // 同步运行期配置副本, 保证后续 save config 导出的是当前生效值
            // (写入被驱动钳位时保存请求值, 重放时钳位结果一致, 幂等)
            if (live_cfg_) {
                if (what == "gain")        live_cfg_->sensor_gain = (int)n;
                else if (what == "exposure") live_cfg_->sensor_exposure = (int)n;
            }
            status_.logInfo(log);
        } else {
            status_.logWarn(log);
        }
    } else if (a == "isp") {
        std::string what = lower(b);
        if (what == "profile") {
            std::string p = lower(c);
            int t = (p == "tl84") ? 1 : (p == "d50") ? 0 : -1;
            if (t < 0) {
                status_.logWarn("[CTRL] usage: isp profile tl84|d50");
                return;
            }
            std::string log;
            if (cam_.setIlluminant(t, log)) {
                if (live_cfg_) {
                    live_cfg_->isp.enabled = true;
                    live_cfg_->isp.illuminant = t;
                }
                status_.logInfo(log);
                if (!cam_.lastIspOutput().empty())
                    printBlock(cam_.lastIspOutput());
            } else {
                status_.logWarn(log);
            }
        } else if (what == "contrast") {
            std::string cv = lower(c);
            int t = -1;
            if (cv == "none") t = 0;
            else if (cv == "50") t = 1;
            else if (cv == "200") t = 2;
            else if (cv == "dynamic") t = 3;
            else if (cv.size() == 1 && cv[0] >= '0' && cv[0] <= '3')
                t = cv[0] - '0';
            if (t < 0) {
                status_.logWarn("[CTRL] usage: isp contrast none|50|200|dynamic");
                return;
            }
            std::string log;
            if (cam_.setContrast(t, log)) {
                if (live_cfg_)
                    live_cfg_->isp.contrast = t;
                status_.logInfo(log);
                if (!cam_.lastIspOutput().empty())
                    printBlock(cam_.lastIspOutput());
            } else {
                status_.logWarn(log);
            }
        } else if (what == "auto-gain") {
            std::string run = lower(c);
            if (run != "run") {
                status_.logWarn("[CTRL] usage: isp auto-gain run");
                return;
            }
            std::string log;
            if (cam_.runAutoGain(log)) {
                status_.logInfo(log);
                if (!cam_.lastIspOutput().empty())
                    printBlock(cam_.lastIspOutput());
            } else {
                status_.logWarn(log);
            }
        } else {
            status_.logWarn("[CTRL] usage: isp profile|contrast|auto-gain ...");
        }
    } else if (a == "dump") {
        std::string what = lower(b);
        if (!dumper_) {
            status_.logWarn("[CTRL] dumper unavailable");
            return;
        }
        if (what == "frame") {
            dumper_->requestNext(1);
            status_.logInfo("[CTRL] dump: next completed frame will be saved");
        } else if (what == "next") {
            char *end = nullptr;
            long long n = strtoll(c.c_str(), &end, 10);
            if (c.empty() || !end || *end != '\0' || n <= 0) {
                status_.logWarn("[CTRL] usage: dump next N (N>0)");
                return;
            }
            dumper_->requestNext((uint64_t)n);
            char msg[64];
            snprintf(msg, sizeof(msg), "[CTRL] dump: next %lld frames will be saved",
                     (long long)n);
            status_.logInfo(msg);
        } else {
            status_.logWarn("[CTRL] usage: dump frame | dump next N");
        }
    } else if (a == "save" || a == "load") {
        handleConfig(a, lower(b) == "config" ? c : std::string());
        if (lower(b) != "config")
            status_.logWarn("[CTRL] usage: save config <file> | load config <file>");
    } else {
        status_.logWarn("[CTRL] unknown command '" + a + "' (try: help)");
    }
}

} // namespace camstream
