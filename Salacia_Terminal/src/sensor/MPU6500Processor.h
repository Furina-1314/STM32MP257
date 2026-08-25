#pragma once

#include "communication/TelemetryPacket.h" // RawImuSample
#include "core/DataManager.h"              // AttitudeData

#include <cstdint>

namespace salacia {

// MPU6500 六轴姿态解算（Mahony 互补滤波，无磁力计——航向存在慢漂移；
// Kp/Ki 经 app_config.ini [imu] 配置，参数解耦红线）
//
// 线程约定：由 UdpReceiver 在遥测工作线程内串行调用（无需加锁）。
// dt 取自岸基接收时间差（限幅 1~200ms，抗丢包抖动）。
class MPU6500Processor
{
public:
    explicit MPU6500Processor(float kp = 0.5F, float ki = 0.0F);

    void setGains(float kp, float ki);

    // 输入一帧原始 IMU，推进滤波并输出姿态（四元数 + 欧拉角）
    AttitudeData process(const RawImuSample& sample);

    void reset();

private:
    void integrate(float ax, float ay, float az,
                   float gx, float gy, float gz, float dt);

    float kp_ = 0.5F;
    float ki_ = 0.0F;

    // 四元数状态（w, x, y, z）
    float q0_ = 1.0F;
    float q1_ = 0.0F;
    float q2_ = 0.0F;
    float q3_ = 0.0F;

    // 积分反馈项
    float integralX_ = 0.0F;
    float integralY_ = 0.0F;
    float integralZ_ = 0.0F;

    qint64 lastHostMs_ = 0;
    bool started_ = false;
};

} // namespace salacia
