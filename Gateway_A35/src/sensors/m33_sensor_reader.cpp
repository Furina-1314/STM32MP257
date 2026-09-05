#include "sensors/m33_sensor_reader.hpp"

#include <cmath>

namespace gw::sensors {

namespace {

float accelToMps2(std::int16_t raw)
{
    return static_cast<float>(static_cast<double>(raw) / kMpuAccelLsbPerG
                              * kGravityMps2);
}

float gyroToRadS(std::int16_t raw)
{
    return static_cast<float>(static_cast<double>(raw) / kMpuGyroLsbPerDps
                              * (kPi / 180.0));
}

std::string rovFailureText(const rov::RovFailure& failure)
{
    // rov::toString lives in the vendor translation unit that host tests do
    // not link; the numeric code plus detail carries the same information.
    return "rov error " + std::to_string(static_cast<int>(failure.code))
            + ": " + failure.detail;
}

} // namespace

M33SensorReader::M33SensorReader(IRovControl& rov, const SensorConfig& config)
    : rov_(rov)
    , config_(config)
{
}

MpuSample M33SensorReader::pollMpu()
{
    MpuSample sample;
    sample.timestamp = SensorClock::now();

    const auto result = rov_.readMpu();
    if (!result) {
        sample.status = SensorStatus::DeviceError;
        sample.errorMessage = rovFailureText(result.failure);
        return sample;
    }
    const rov::MpuRaw& raw = *result.value;
    sample.accelMps2[0] = accelToMps2(raw.ax);
    sample.accelMps2[1] = accelToMps2(raw.ay);
    sample.accelMps2[2] = accelToMps2(raw.az);
    sample.gyroRadS[0] = gyroToRadS(raw.gx);
    sample.gyroRadS[1] = gyroToRadS(raw.gy);
    sample.gyroRadS[2] = gyroToRadS(raw.gz);
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(sample.accelMps2[i])
            || !std::isfinite(sample.gyroRadS[i])) {
            sample.status = SensorStatus::DeviceError;
            sample.errorMessage = "non-finite mpu conversion";
            return sample;
        }
    }
    sample.status = SensorStatus::Ok;
    return sample;
}

DypSample M33SensorReader::pollDyp()
{
    DypSample sample;
    sample.timestamp = SensorClock::now();
    sample.distanceMm = -1.0F;

    const auto result = rov_.readDyp();
    if (!result) {
        sample.status = SensorStatus::DeviceError;
        sample.errorMessage = rovFailureText(result.failure);
        return sample;
    }
    const float distance = static_cast<float>(result.value->distanceMm);
    if ((distance < config_.dypValidMinMm) || (distance > config_.dypValidMaxMm)) {
        // A UART-valid frame is not necessarily a physical distance
        // (65533-class values are not an acknowledged sentinel - D-10).
        sample.status = SensorStatus::OutOfRange;
        sample.errorMessage = "dyp distance outside physical window: "
                + std::to_string(distance);
        return sample;
    }
    sample.distanceMm = distance;
    sample.valid = true;
    sample.status = SensorStatus::Ok;
    return sample;
}

} // namespace gw::sensors
