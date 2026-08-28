#include "dcmipp_setup.h"

#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include "sensor_control.h"

namespace camstream {

// ====== DCMIPP media 管线配置 =====
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
std::string find_subdev(const char *entity_hint) {
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

bool setup_dcmipp_pipeline(uint32_t sensor_w, uint32_t sensor_h,
                           uint32_t out_w, uint32_t out_h,
                           int sensor_gain, int sensor_exposure,
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
    // 的 subdev 节点再设置, 避免编号漂移导致设置落空.
    // (plan §4.8/§4.9) SensorControl ioctl 直控 + 钳位 + 回读, 打印 actual
    std::string sensor = find_subdev("imx335");
    sensor_out = sensor;
    if (!sensor.empty()) {
        std::cout << "Sensor: " << sensor << std::endl;
        SensorControl sc;
        if (sc.openNode(sensor)) {
            int64_t actual = 0;
            bool clamped = false;
            std::string em;
            if (sc.set("analogue_gain", sensor_gain, actual, clamped, em)) {
                std::cout << "[SENSOR] gain      : requested=" << sensor_gain
                          << " actual=" << actual;
                if (clamped) std::cout << " (clamped)";
                if (!em.empty()) std::cout << "  [" << em << "]";
                std::cout << std::endl;
            } else {
                std::cerr << "[WARN] set analogue_gain failed: " << em << std::endl;
            }
            if (sc.set("exposure", sensor_exposure, actual, clamped, em)) {
                std::cout << "[SENSOR] exposure  : requested=" << sensor_exposure
                          << " actual=" << actual;
                if (clamped) std::cout << " (clamped)";
                if (!em.empty()) std::cout << "  [" << em << "]";
                std::cout << std::endl;
            } else {
                std::cerr << "[WARN] set exposure failed: " << em << std::endl;
            }
        } else {
            std::cerr << "[WARN] open sensor node failed: "
                      << sc.lastError() << std::endl;
        }
    } else {
        std::cerr << "WARN: imx335 subdev not found, sensor gain/exposure NOT set"
                  << std::endl;
    }
    std::string isp_node = find_subdev("dcmipp_main_isp");
    if (!isp_node.empty()) {
        SensorControl isc;
        if (isc.openNode(isp_node)) {
            int64_t actual = 0;
            bool clamped = false;
            std::string em;
            if (isc.set("gamma_correction", 1, actual, clamped, em)) {
                std::cout << "[SENSOR] gamma     : actual=" << actual << std::endl;
            } else if (em.find("not found") != std::string::npos) {
                // 与原实现行为对齐: 原 system() 调用本就静默忽略失败(丢弃
                // stderr 且不查返回值), 本板 ISP subdev 无此控件属正常情况
                std::cout << "[SENSOR] gamma     : skipped (control not present "
                             "on this ISP subdev)" << std::endl;
            } else {
                std::cerr << "[WARN] set gamma_correction failed: "
                          << em << std::endl;
            }
        }
    }

    std::cout << "DCMIPP pipeline configured: sensor " << sensor_w << "x" << sensor_h
              << " -> output " << out_w << "x" << out_h << std::endl;
    return true;
}

// ====== 传感器启动时序(必须在 STREAMON 之前调用) ======
// 板级校准模型: actual_fps = SENSOR_CAL_K / (SENSOR_CAL_C + vblank)
// 由两组实测拟合(vblank 2560 -> 30.000fps, 13508 -> 8.739fps), 残差 <0.1%;
// 驱动上报 pixel_rate/hblank 与真实时序不一致(同 vblank 下公式偏快 1.4~1.6
// 倍), 故不再用上报值推算. 常数仅对本板 IMX335 1080p SRGGB10 模式有效.
static const double SENSOR_CAL_K = 135042.0;
static const double SENSOR_CAL_C = 1941.0;

bool prepare_sensor_timing(const std::string &subdev, int target_fps) {
    if (subdev.empty()) {
        std::cerr << "WARN: sensor node unknown, skip sensor timing setup" << std::endl;
        return false;
    }
    int fd = open(subdev.c_str(), O_RDWR);
    if (fd < 0) {
        std::cerr << "WARN: open " << subdev << " for timing setup failed" << std::endl;
        return false;
    }

    // vblank 范围/默认值
    struct v4l2_query_ext_ctrl qc;
    memset(&qc, 0, sizeof(qc));
    qc.id = V4L2_CID_VBLANK;
    if (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &qc) < 0) {
        std::cerr << "WARN: query vblank range failed: " << strerror(errno) << std::endl;
        close(fd);
        return false;
    }

    // 当前 vblank
    struct v4l2_ext_control ec;
    struct v4l2_ext_controls ecs;
    memset(&ec, 0, sizeof(ec));
    ec.id = V4L2_CID_VBLANK;
    memset(&ecs, 0, sizeof(ecs));
    ecs.ctrl_class = V4L2_CTRL_CLASS_IMAGE_SOURCE;
    ecs.count = 1;
    ecs.controls = &ec;
    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &ecs) < 0) {
        std::cerr << "WARN: read vblank failed: " << strerror(errno) << std::endl;
        close(fd);
        return false;
    }
    const int64_t cur = ec.value;

    // 目标 vblank: 不降帧 -> 驱动默认值(实测 = 30fps 模式默认节拍);
    //              显式帧率 -> 校准模型反解
    int64_t want;
    double want_fps;
    if (target_fps > 0) {
        want_fps = (double)target_fps;
        want = (int64_t)(SENSOR_CAL_K / want_fps - SENSOR_CAL_C + 0.5);
    } else {
        want = qc.default_value;
        want_fps = SENSOR_CAL_K / (SENSOR_CAL_C + (double)want);
    }
    if (want < qc.minimum) want = qc.minimum;
    if (want > qc.maximum) want = qc.maximum;

    if (want == cur) {
        std::cout << "Sensor timing: vblank " << cur << " already at target ("
                  << want_fps << "fps)" << std::endl;
        close(fd);
        return true;
    }

    bool ok = false;
    memset(&ec, 0, sizeof(ec));
    ec.id = V4L2_CID_VBLANK;
    ec.value = (int32_t)want;
    memset(&ecs, 0, sizeof(ecs));
    ecs.ctrl_class = V4L2_CTRL_CLASS_IMAGE_SOURCE;
    ecs.count = 1;
    ecs.controls = &ec;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ecs) == 0) {
        // 回读校验: 确认控制值真正落位(STREAMON 时才会应用到硬件)
        memset(&ec, 0, sizeof(ec));
        ec.id = V4L2_CID_VBLANK;
        memset(&ecs, 0, sizeof(ecs));
        ecs.ctrl_class = V4L2_CTRL_CLASS_IMAGE_SOURCE;
        ecs.count = 1;
        ecs.controls = &ec;
        int64_t now = cur;
        if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &ecs) == 0)
            now = ec.value;
        if (now != want)
            std::cerr << "WARN: vblank readback " << now << " != requested "
                      << want << std::endl;
        else
            ok = true;
        std::cout << "Sensor timing: vblank " << cur << " -> " << want
                  << " (" << want_fps << "fps target, "
                  << (target_fps > 0 ? "calibrated" : "driver default") << ")"
                  << std::endl;
    }
    else
        std::cerr << "WARN: set vblank failed: " << strerror(errno) << std::endl;
    close(fd);
    return ok;
}

} // namespace camstream
