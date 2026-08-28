#include "camera_control.h"

#include <cstdio>

#include "sensor_control.h"

namespace camstream {

CameraControl::CameraControl(const std::string &sensor_node,
                             const IspStartupConfig &isp_cfg)
    : sensor_node_(sensor_node), isp_(isp_cfg) {}

bool CameraControl::setSensorCtrl(const char *name, int64_t value,
                                  std::string &log) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sensor_node_.empty()) {
        log = std::string("[CTRL] sensor ") + name + ": sensor node unknown";
        return false;
    }
    SensorControl sc;
    if (!sc.openNode(sensor_node_)) {
        log = std::string("[CTRL] sensor ") + name + ": open failed: " +
              sc.lastError();
        return false;
    }
    int64_t cur = value;
    (void)sc.get(name, cur);            // 尽力读取旧值(失败则显示请求值)
    int64_t actual = value;
    bool clamped = false;
    std::string em;
    if (!sc.set(name, value, actual, clamped, em)) {
        log = std::string("[CTRL] sensor ") + name + " -> " +
              std::to_string((long long)value) + " FAILED: " + em;
        return false;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "[CTRL] sensor %s: %lld -> %lld (actual %lld)%s",
             name, (long long)cur, (long long)value, (long long)actual,
             clamped ? " (clamped)" : "");
    log = buf;
    return true;
}

bool CameraControl::setSensorGain(int64_t v, std::string &log) {
    return setSensorCtrl("analogue_gain", v, log);
}

bool CameraControl::setSensorExposure(int64_t v, std::string &log) {
    return setSensorCtrl("exposure", v, log);
}

bool CameraControl::setSensorVBlank(int64_t v, std::string &log) {
    bool ok = setSensorCtrl("vertical_blanking", v, log);
    if (ok)
        log += " [takes effect at next STREAMON]";
    return ok;
}

bool CameraControl::getSensor(const char *name, int64_t &value, std::string &log) {
    std::lock_guard<std::mutex> lock(mutex_);
    value = 0;
    if (sensor_node_.empty()) {
        log = std::string("[CTRL] sensor ") + name + ": sensor node unknown";
        return false;
    }
    SensorControl sc;
    if (!sc.openNode(sensor_node_) || !sc.get(name, value)) {
        log = std::string("[CTRL] sensor ") + name + ": read failed";
        return false;
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "[CTRL] sensor %s: %lld", name, (long long)value);
    log = buf;
    return true;
}

bool CameraControl::setIlluminant(int type, std::string &log) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char *from = IspController::illuminantName(isp_.config().illuminant);
    IspApplyResult r = isp_.setIlluminant(type);
    isp_output_ = r.output;
    if (!r.ran || r.exit_code != 0) {
        log = std::string("[CTRL] ISP illuminant -> ") +
              IspController::illuminantName(type) + " FAILED" +
              (r.ran ? " (exit=" + std::to_string(r.exit_code) + ")"
                     : " (" + r.output + ")");
        return false;
    }
    log = std::string("[CTRL] ISP illuminant: ") + from + " -> " +
          IspController::illuminantName(type) + " [OK]";
    return true;
}

bool CameraControl::setContrast(int type, std::string &log) {
    std::lock_guard<std::mutex> lock(mutex_);
    IspApplyResult r = isp_.setContrast(type);
    isp_output_ = r.output;
    if (!r.ran || r.exit_code != 0) {
        log = std::string("[CTRL] ISP contrast -> ") +
              IspController::contrastName(type) + " FAILED" +
              (r.ran ? " (exit=" + std::to_string(r.exit_code) + ")"
                     : " (" + r.output + ")");
        return false;
    }
    log = std::string("[CTRL] ISP contrast: ") +
          IspController::contrastName(type) + " [OK]";
    return true;
}

bool CameraControl::runAutoGain(std::string &log) {
    std::lock_guard<std::mutex> lock(mutex_);
    IspApplyResult r = isp_.runAutoGain();
    isp_output_ = r.output;
    if (!r.ran || r.exit_code != 0) {
        log = "[CTRL] ISP auto-gain FAILED" +
              (r.ran ? " (exit=" + std::to_string(r.exit_code) + ")"
                     : " (" + r.output + ")");
        return false;
    }
    log = "[CTRL] ISP auto-gain [OK]";
    return true;
}

} // namespace camstream
