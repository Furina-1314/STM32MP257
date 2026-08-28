/*
 * dcmipp_setup.h - DCMIPP media 管线配置与传感器帧率设置
 *
 * 从原 main 逐字搬运(Phase 1 行为不变):
 *   - find_subdev          : 从 media-ctl -p 解析实体对应的 /dev/v4l-subdevN
 *   - setup_dcmipp_pipeline: media-ctl 重建链路/格式 + sensor gain/exposure + gamma
 *   - set_sensor_fps       : 按 vblank 调节传感器帧间隔(ioctl + 回读)
 */

#ifndef CAMSTREAM_DCMIPP_SETUP_H
#define CAMSTREAM_DCMIPP_SETUP_H

#include <string>
#include <cstdint>

namespace camstream {

// 从 media-ctl -p 输出解析实体对应的 /dev/v4l-subdevN
// (subdev 编号跨重启会漂移, 不能硬编码)
std::string find_subdev(const char *entity_hint);

// DCMIPP media 管线配置(重启后内核复位为 640x480, 不重配则 STREAMON 失败):
// 传感器->CSI->input 为 SRGGB10, ISP 输出起为 RGB888_1X24;
// postproc crop 全幅、compose 到输出分辨率(输出小于传感器时由其硬件缩放).
// sensor_out 返回 imx335 的 subdev 节点; sensor_gain/sensor_exposure 经
// SensorControl ioctl 直控写入并回读(不再经 v4l2-ctl/shell).
bool setup_dcmipp_pipeline(uint32_t sensor_w, uint32_t sensor_h,
                           uint32_t out_w, uint32_t out_h,
                           int sensor_gain, int sensor_exposure,
                           std::string &sensor_out);

// ====== 传感器启动时序(必须在 STREAMON 之前调用) ======
// 实测约束(2026-08-27/28 板上日志):
//   1. IMX335 驱动只在启动流时应用 vblank, 流状态下写入不生效;
//   2. vblank 控制值跨运行持久, 旧运行残留的节拍值若不清除, 会被下次
//      STREAMON 应用导致帧率错误(实测残留 13508 -> 8.7fps);
//   3. 驱动上报的 pixel_rate/hblank 与真实时序不一致, 按上报值计算的
//      fps 公式系统性偏快 1.4~1.6 倍(vblank 2560: 公式 48.1/实测 30.0;
//      vblank 13508: 公式 12.0/实测 8.74), 故改用板级校准模型.
// 行为:
//   target_fps <= 0 : 恢复 vblank 为驱动默认值(本板 = 模式默认 30fps 节拍,
//                     "不降帧"默认策略, 处理链自然限流);
//   target_fps > 0  : 按校准模型 actual_fps = K/(C+vblank) 反解 vblank.
// 返回 true=已处于目标值或写入并回读成功; 失败仅告警, 不阻塞启动.
bool prepare_sensor_timing(const std::string &subdev, int target_fps);

} // namespace camstream

#endif // CAMSTREAM_DCMIPP_SETUP_H
