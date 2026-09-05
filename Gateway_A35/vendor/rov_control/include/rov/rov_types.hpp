#ifndef ROV_ROV_TYPES_HPP
#define ROV_ROV_TYPES_HPP

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

namespace rov {

struct DypReading {
    std::uint16_t distanceMm{0};

    // A valid DYP UART frame does not necessarily mean a physically valid
    // distance. This API preserves the M33 value without applying a range
    // policy; for example, 65533 is intentionally returned unchanged.
};

struct MpuRaw {
    std::int16_t ax{0};
    std::int16_t ay{0};
    std::int16_t az{0};
    std::int16_t gx{0};
    std::int16_t gy{0};
    std::int16_t gz{0};
};

enum class DypState {
    Uninitialized,
    Idle,
    Waiting,
    Complete,
    Timeout,
    IoError
};

struct SensorSnapshot {
    bool mpuReady{false};
    bool dypReady{false};
    DypState dypState{DypState::Uninitialized};
    bool dypBusy{false};
    std::optional<std::uint16_t> distanceMm;
    std::optional<std::chrono::milliseconds> age;
};

struct Attitude {
    double rollDegrees{0.0};
    double pitchDegrees{0.0};
    bool ready{false};
};

struct StabilizationStatus {
    double rollErrorDegrees{0.0};
    double pitchErrorDegrees{0.0};
    double rollPidCommand{0.0};
    double pitchPidCommand{0.0};
    std::array<std::int16_t, 4> verticalCorrections{};
    bool attitudeReady{false};
    bool attitudeFresh{false};
    bool horizontalEnabled{false};
    bool globalStopped{false};
    bool verticalStopped{false};
    bool horizontalStopped{false};
};

} // namespace rov

#endif // ROV_ROV_TYPES_HPP
