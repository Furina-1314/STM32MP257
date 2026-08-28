#include "camera_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace camstream {

static std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static const char *profileName(const IspStartupConfig &isp) {
    if (!isp.enabled) return "none";
    return IspController::illuminantName(isp.illuminant);
}

static const char *contrastName(const IspStartupConfig &isp) {
    return IspController::contrastName(isp.contrast);
}

bool CameraConfig::save(const std::string &path, const AppConfig &cfg) {
    std::ofstream f(path.c_str());
    if (!f.is_open())
        return false;
    f << "# camstream runtime config\n"
      << "sensor_gain=" << cfg.sensor_gain << "\n"
      << "sensor_exposure=" << cfg.sensor_exposure << "\n"
      << "isp_profile=" << profileName(cfg.isp) << "\n"
      << "isp_contrast=" << contrastName(cfg.isp) << "\n"
      << "input_mode=" << (cfg.input_copy ? "copy" : "hostptr") << "\n"
      << "colorimetry=" << (cfg.colorimetry == 1 ? "bt709" : "bt601") << "\n"
      << "bitrate_kbps=" << cfg.bitrate_kbps << "\n"
      << "gop=" << cfg.gop << "\n"
      << "appsrc_queue=" << cfg.appsrc_queue << "\n"
      << "fps=" << cfg.target_fps << "\n";
    return f.good();
}

bool CameraConfig::load(const std::string &path, AppConfig &cfg) {
    std::ifstream f(path.c_str());
    if (!f.is_open())
        return false;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            fprintf(stderr, "[WARN] config: ignore malformed line '%s'\n",
                    line.c_str());
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (key == "sensor_gain") {
            cfg.sensor_gain = atoi(val.c_str());
        } else if (key == "sensor_exposure") {
            cfg.sensor_exposure = atoi(val.c_str());
        } else if (key == "isp_profile") {
            if (val == "tl84")      { cfg.isp.enabled = true; cfg.isp.illuminant = 1; }
            else if (val == "d50")  { cfg.isp.enabled = true; cfg.isp.illuminant = 0; }
            else if (val == "none") { cfg.isp.enabled = false; }
            else fprintf(stderr, "[WARN] config: unknown isp_profile '%s'\n", val.c_str());
        } else if (key == "isp_contrast") {
            if (val == "none") cfg.isp.contrast = 0;
            else if (val == "50") cfg.isp.contrast = 1;
            else if (val == "200") cfg.isp.contrast = 2;
            else if (val == "dynamic") cfg.isp.contrast = 3;
            else if (val == "unset") cfg.isp.contrast = -1;
            else fprintf(stderr, "[WARN] config: unknown isp_contrast '%s'\n", val.c_str());
        } else if (key == "input_mode") {
            cfg.input_copy = (val == "copy");
        } else if (key == "colorimetry") {
            cfg.colorimetry = (val == "bt709") ? 1 : 0;
        } else if (key == "bitrate_kbps") {
            cfg.bitrate_kbps = atoi(val.c_str());
        } else if (key == "gop") {
            cfg.gop = atoi(val.c_str());
        } else if (key == "appsrc_queue") {
            cfg.appsrc_queue = atoi(val.c_str());
        } else if (key == "fps") {
            cfg.target_fps = atoi(val.c_str());
        } else {
            fprintf(stderr, "[WARN] config: unknown key '%s' ignored\n", key.c_str());
        }
    }
    return true;
}

} // namespace camstream
