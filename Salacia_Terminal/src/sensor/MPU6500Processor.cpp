#include "MPU6500Processor.h"

#include <cmath>
#include <utility>

namespace salacia {

namespace {
constexpr float kDegPerRad = 57.29577951308232F;

// Mahony 增益限幅（配置越界时的硬边界，属算法安全域而非业务参数）
constexpr float kMaxKp = 10.0F;
constexpr float kMaxKi = 10.0F;

// dt 限幅（秒）：抗丢包/突发造成的异常时间差
constexpr float kMinDtSec = 0.001F;
constexpr float kMaxDtSec = 0.2F;

float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}
} // namespace

MPU6500Processor::MPU6500Processor(float kp, float ki)
{
    setGains(kp, ki);
}

void MPU6500Processor::setGains(float kp, float ki)
{
    kp_ = clampf(kp, 0.0F, kMaxKp);
    ki_ = clampf(ki, 0.0F, kMaxKi);
}

void MPU6500Processor::reset()
{
    q0_ = 1.0F;
    q1_ = 0.0F;
    q2_ = 0.0F;
    q3_ = 0.0F;
    integralX_ = 0.0F;
    integralY_ = 0.0F;
    integralZ_ = 0.0F;
    started_ = false;
    lastHostMs_ = 0;
}

RovState MPU6500Processor::process(const RawImuSample& sample)
{
    RovState out;
    out.cabinTempC = sample.cabinTempC;
    out.cabinHumidityPct = sample.cabinHumidityPct;
    out.batteryVoltage = sample.batteryVoltage;
    out.timestampMs = static_cast<qint64>(sample.hostTimeMs);

    // 首帧：仅初始化时间基准
    if (!started_) {
        started_ = true;
        lastHostMs_ = static_cast<qint64>(sample.hostTimeMs);
        return out;
    }

    const qint64 deltaMs =
            static_cast<qint64>(sample.hostTimeMs) - lastHostMs_;
    lastHostMs_ = static_cast<qint64>(sample.hostTimeMs);
    if (deltaMs <= 0) {
        // 同毫秒重复帧/时钟回拨：跳过积分，但返回上一姿态（默认构造的
        // out 是 0°，返回它会以 100Hz 闪回水平姿态）
        return lastOut_;
    }
    const float dt = clampf(static_cast<float>(deltaMs) / 1000.0F,
                            kMinDtSec, kMaxDtSec);

    integrate(sample.accelMps2[0], sample.accelMps2[1], sample.accelMps2[2],
              sample.gyroRadS[0], sample.gyroRadS[1], sample.gyroRadS[2], dt);

    out.quaternion[0] = q0_;
    out.quaternion[1] = q1_;
    out.quaternion[2] = q2_;
    out.quaternion[3] = q3_;

    // ZYX 欧拉角（度）
    const float sinrCosp = 2.0F * (q0_ * q1_ + q2_ * q3_);
    const float cosrCosp = 1.0F - 2.0F * (q1_ * q1_ + q2_ * q2_);
    out.rollDeg = std::atan2(sinrCosp, cosrCosp) * kDegPerRad;

    const float sinp = 2.0F * (q0_ * q2_ - q3_ * q1_);
    out.pitchDeg = std::asin(clampf(sinp, -1.0F, 1.0F)) * kDegPerRad;

    const float sinyCosp = 2.0F * (q0_ * q3_ + q1_ * q2_);
    const float cosyCosp = 1.0F - 2.0F * (q2_ * q2_ + q3_ * q3_);
    out.yawDeg = std::atan2(sinyCosp, cosyCosp) * kDegPerRad;

    lastOut_ = out;
    return out;
}

void MPU6500Processor::integrate(float ax, float ay, float az,
                                 float gx, float gy, float gz, float dt)
{
    // 加速度归一化（仅方向参与修正；自由落体/强加速度时段误差增大）
    const float norm = std::sqrt(ax * ax + ay * ay + az * az);
    if (norm < 1.0e-6F) {
        return; // 无效加速度：仅积分陀螺（走下方公共路径）
    }
    const float invNorm = 1.0F / norm;
    ax *= invNorm;
    ay *= invNorm;
    az *= invNorm;

    // 估计的重力方向（四元数旋转第三轴）
    const float halfVx = q1_ * q3_ - q0_ * q2_;
    const float halfVy = q0_ * q1_ + q2_ * q3_;
    const float halfVz = q0_ * q0_ - 0.5F + q3_ * q3_;

    // 误差 = 测量重力 × 估计重力
    const float halfEx = ay * halfVz - az * halfVy;
    const float halfEy = az * halfVx - ax * halfVz;
    const float halfEz = ax * halfVy - ay * halfVx;

    if (ki_ > 0.0F) {
        integralX_ += halfEx * ki_ * dt;
        integralY_ += halfEy * ki_ * dt;
        integralZ_ += halfEz * ki_ * dt;
        gx += integralX_;
        gy += integralY_;
        gz += integralZ_;
    }
    gx += kp_ * halfEx;
    gy += kp_ * halfEy;
    gz += kp_ * halfEz;

    // 四元数速率积分
    const float dq0 = 0.5F * (-q1_ * gx - q2_ * gy - q3_ * gz);
    const float dq1 = 0.5F * (q0_ * gx + q2_ * gz - q3_ * gy);
    const float dq2 = 0.5F * (q0_ * gy - q1_ * gz + q3_ * gx);
    const float dq3 = 0.5F * (q0_ * gz + q1_ * gy - q2_ * gx);

    q0_ += dq0 * dt;
    q1_ += dq1 * dt;
    q2_ += dq2 * dt;
    q3_ += dq3 * dt;

    // 归一化
    const float qn = std::sqrt(q0_ * q0_ + q1_ * q1_ + q2_ * q2_ + q3_ * q3_);
    if (qn > 1.0e-6F) {
        const float inv = 1.0F / qn;
        q0_ *= inv;
        q1_ *= inv;
        q2_ *= inv;
        q3_ *= inv;
    }
}

} // namespace salacia
