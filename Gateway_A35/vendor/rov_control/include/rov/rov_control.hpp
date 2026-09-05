#ifndef ROV_ROV_CONTROL_HPP
#define ROV_ROV_CONTROL_HPP

#include "rov/rov_result.hpp"
#include "rov/rov_types.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace rov {
namespace internal {
class ITransport;
class RovControlTestAccess;
} // namespace internal

class RovControl final {
public:
    explicit RovControl(std::string device = "/dev/ttyRPMSG0");
    ~RovControl();

    RovControl(const RovControl&) = delete;
    RovControl& operator=(const RovControl&) = delete;

    RovResult<void> open();
    void close() noexcept;
    bool isOpen() const noexcept;

    RovResult<void> setServo(std::uint8_t id, std::uint8_t angle);
    RovResult<void> setAllServos(std::uint8_t angle);
    RovResult<void> centerServo(std::uint8_t id);
    RovResult<void> centerAllServos();
    RovResult<std::uint8_t> getServo(std::uint8_t id);
    RovResult<std::array<std::uint8_t, 10>> getAllServos();

    // Base behavior is defined by the M33 stabilization/horizontal mode. The
    // A35 client deliberately forwards the command without changing semantics.
    RovResult<void> setVerticalBase(std::int16_t value);
    // M33 may reject an individual command with err safety.
    RovResult<void> setVerticalPropeller(std::uint8_t id,
                                         std::int16_t value);
    // Base behavior is defined by the M33 horizontal synchronization mode.
    RovResult<void> setHorizontalBase(std::int16_t value);
    // M33 may reject an individual command with err safety.
    RovResult<void> setHorizontalPropeller(std::uint8_t id,
                                           std::int16_t value);

    RovResult<std::int16_t> getPropellerBase(std::uint8_t id);
    // "real" is the M33 software-recorded output command. It is neither a
    // PCA9685 readback nor measured propeller RPM feedback.
    RovResult<std::int16_t> getPropellerOutput(std::uint8_t id);
    RovResult<std::array<std::int16_t, 6>> getAllPropellerBases();
    // Values are M33 software-recorded commands ordered CH10 through CH15.
    RovResult<std::array<std::int16_t, 6>> getAllPropellerOutputs();

    // These APIs map to the legacy M33 wire command "horizontal on/off".
    RovResult<void> enableStabilization();
    RovResult<void> disableStabilization();
    RovResult<void> enableHorizontalSynchronization();
    RovResult<void> disableHorizontalSynchronization();

    RovResult<DypReading> readDyp();
    RovResult<MpuRaw> readMpu();
    RovResult<SensorSnapshot> getSensorSnapshot();
    RovResult<Attitude> getAttitude();
    RovResult<StabilizationStatus> getStabilization();

    RovResult<void> stop();
    RovResult<void> move();
    RovResult<void> stopVertical();
    RovResult<void> moveVertical();
    RovResult<void> stopHorizontal();
    RovResult<void> moveHorizontal();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    explicit RovControl(std::unique_ptr<internal::ITransport> transport);
    friend class internal::RovControlTestAccess;
};

} // namespace rov

#endif // ROV_ROV_CONTROL_HPP
