/*
 * camera_control.h - 运行时统一控制入口(plan §10)
 *
 * 两个命名空间严格分开, 禁止模糊的单一 "gain"(plan §11.2):
 *   sensor.* : IMX335 subdev 控件, SensorControl ioctl 直控 + 回读
 *              (analogue_gain / exposure / vertical_blanking)
 *   isp.*    : dcmipp-isp-ctrl 子进程(illuminant/contrast/auto-gain)
 *
 * 所有 setter 遵循: 查范围 -> 钳位 -> 写入 -> 回读 -> 生成 [CTRL] 日志行.
 * 内部互斥锁串行化硬件操作, 供控制台线程与主循环并发使用.
 */

#ifndef CAMSTREAM_CAMERA_CONTROL_H
#define CAMSTREAM_CAMERA_CONTROL_H

#include <string>
#include <mutex>

#include "isp_controller.h"

namespace camstream {

class CameraControl {
public:
    CameraControl(const std::string &sensor_node, const IspStartupConfig &isp_cfg);

    // ---- sensor(写入+回读; 成功返回 true, log 为 [CTRL] 日志行) ----
    bool setSensorGain(int64_t value, std::string &log);
    bool setSensorExposure(int64_t value, std::string &log);
    // 注意: vblank 流状态下写入不生效(IMX335 驱动在启动流时应用, 见
    // prepare_sensor_timing 注释), 日志中会提示
    bool setSensorVBlank(int64_t value, std::string &log);

    // 读传感器控件值(log 为 "[CTRL] sensor gain: N" 风格)
    bool getSensor(const char *ctrl_name, int64_t &value, std::string &log);

    // ---- isp helper ----
    bool setIlluminant(int type, std::string &log);   // 0=D50 1=TL84
    bool setContrast(int type, std::string &log);     // 0=none..3=dynamic
    bool runAutoGain(std::string &log);

    // 最近一次 isp 子进程输出(控制台多行打印用)
    const std::string &lastIspOutput() const { return isp_output_; }

    bool sensorReady() const { return !sensor_node_.empty(); }
    const std::string &sensorNode() const { return sensor_node_; }
    const IspStartupConfig &ispConfig() const { return isp_.config(); }

private:
    bool setSensorCtrl(const char *name, int64_t value, std::string &log);

    std::string sensor_node_;
    IspController isp_;
    std::string isp_output_;
    std::mutex mutex_;
};

} // namespace camstream

#endif // CAMSTREAM_CAMERA_CONTROL_H
