#ifndef GW_SENSORS_SENSOR_TYPES_HPP
#define GW_SENSORS_SENSOR_TYPES_HPP

#include <chrono>
#include <cstdint>
#include <string>

namespace gw::sensors {

// Common per-sensor outcome. Slow sensor failures degrade their own
// validMask bit only; they never take the gateway down.
enum class SensorStatus
{
    Ok,
    NotFound,        // sysfs node absent (DHT11: check DS18B20 GPIO conflict)
    ReadError,       // open/read failed
    ParseError,      // non-numeric / malformed content
    AmbiguousFormat, // DHT11 2-3 digit data without a compat policy
    OutOfRange,      // value outside configured physical bounds
    DeviceError,     // M33-side failure (MPU/DYP busy/timeout/io)
};

using SensorClock = std::chrono::steady_clock;
using SensorTime = SensorClock::time_point;

struct Dht11Sample
{
    float temperatureC = 0.0F;
    float humidityPercent = 0.0F;
    std::string rawValue;
    SensorTime timestamp{};
    SensorStatus status = SensorStatus::NotFound;
    bool inferred = false; // three-digit compat parse was applied
    std::string errorMessage;
    bool ok() const { return status == SensorStatus::Ok; }
};

struct Ina226Sample
{
    float busVoltageV = 0.0F;
    float shuntVoltageMv = 0.0F;
    float currentA = 0.0F;
    float powerW = 0.0F;
    std::int64_t shuntResistorUohm = 0;
    bool hasShuntVoltage = false;
    bool hasCurrent = false;
    bool hasPower = false;
    bool hasShuntResistor = false;
    SensorTime timestamp{};
    SensorStatus status = SensorStatus::NotFound;
    std::string errorMessage;
    bool ok() const { return status == SensorStatus::Ok; }
};

struct MpuSample
{
    float accelMps2[3] = {0.0F, 0.0F, 0.0F};
    float gyroRadS[3] = {0.0F, 0.0F, 0.0F};
    SensorTime timestamp{};
    SensorStatus status = SensorStatus::DeviceError;
    std::string errorMessage;
    bool ok() const { return status == SensorStatus::Ok; }
};

struct DypSample
{
    float distanceMm = -1.0F; // invalid sentinel on the wire
    bool valid = false;
    SensorTime timestamp{};
    SensorStatus status = SensorStatus::DeviceError;
    std::string errorMessage;
    bool ok() const { return status == SensorStatus::Ok; }
};

// DHT11 interface selection (Drivers.md section 7).
enum class Dht11Mode
{
    AlientekMisc, // /sys/class/misc/dht11/value (factory firmware default)
    StandardIio,  // /sys/bus/iio/devices/iio:device*/name == "dht11"
    Auto,         // misc first, then IIO
};

struct SensorConfig
{
    std::string sysfsRoot = "/sys"; // injectable for virtual-sysfs tests
    Dht11Mode dht11Mode = Dht11Mode::Auto;
    int dht11PeriodMs = 2000;       // manual default; clamped to >= 1000
    bool dht11ThreeDigitCompat = false;
    int ina226PeriodMs = 500;
    int mpuPeriodMs = 20;   // 50 Hz from M33
    int dypPeriodMs = 200;  // 5 Hz trigger cadence floor ~34 ms + busy guard
    int staleFactor = 3;    // stale after N missed periods
    // Physical bounds (PDF 4.25: 20-90 %RH / 0-50 C; outer bounds used to
    // reject garbage while tolerating sensor tolerance).
    float dht11TempMinC = 0.0F;
    float dht11TempMaxC = 50.0F;
    float dht11HumidityMinPct = 0.0F;
    float dht11HumidityMaxPct = 100.0F;
    float busVoltageMinV = 0.0F;
    float busVoltageMaxV = 36.0F;
    // DYP validity window (D-10; 65533-style UART-only values fall outside).
    float dypValidMinMm = 20.0F;
    float dypValidMaxMm = 8000.0F;
    int summaryPeriodMs = 10; // 100 Hz SensorSummary stream
};

// MPU6500 conversion constants for the current M33 configuration
// (ACCEL_CONFIG=0x00 -> +/-2g, 16384 LSB/g; GYRO_CONFIG=0x00 -> +/-250 dps,
// 131 LSB/dps; verified against the M33 source in phase 0).
constexpr double kMpuAccelLsbPerG = 16384.0;
constexpr double kMpuGyroLsbPerDps = 131.0;
constexpr double kGravityMps2 = 9.80665;
constexpr double kPi = 3.14159265358979323846;

} // namespace gw::sensors

#endif // GW_SENSORS_SENSOR_TYPES_HPP
