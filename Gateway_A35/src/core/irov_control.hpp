#ifndef GW_CORE_IROV_CONTROL_HPP
#define GW_CORE_IROV_CONTROL_HPP

#include "rov/rov_control.hpp"

#include <array>
#include <cstdint>

namespace gw {

// Command-surface seam between the gateway core and the M33. Signatures
// mirror rov::RovControl's public command API one-to-one (RovControl API
// v1), so the phase-4 adapter is a thin forwarder onto the real client and
// tests run against FakeRovControl. Lifecycle (open/close/isOpen) is
// deliberately excluded: the owner opens the real client before wiring it
// into the core.
//
// Error mapping to wire error codes lives in gateway_core (DECISIONS D-06).
class IRovControl
{
public:
    virtual ~IRovControl() = default;

    virtual rov::RovResult<void> setServo(std::uint8_t id, std::uint8_t angle) = 0;
    virtual rov::RovResult<void> setAllServos(std::uint8_t angle) = 0;
    virtual rov::RovResult<void> centerServo(std::uint8_t id) = 0;
    virtual rov::RovResult<void> centerAllServos() = 0;
    virtual rov::RovResult<std::uint8_t> getServo(std::uint8_t id) = 0;
    virtual rov::RovResult<std::array<std::uint8_t, 10>> getAllServos() = 0;

    virtual rov::RovResult<void> setVerticalBase(std::int16_t value) = 0;
    virtual rov::RovResult<void> setVerticalPropeller(std::uint8_t id,
                                                      std::int16_t value) = 0;
    virtual rov::RovResult<void> setHorizontalBase(std::int16_t value) = 0;
    virtual rov::RovResult<void> setHorizontalPropeller(std::uint8_t id,
                                                        std::int16_t value) = 0;

    virtual rov::RovResult<std::int16_t> getPropellerBase(std::uint8_t id) = 0;
    virtual rov::RovResult<std::int16_t> getPropellerOutput(std::uint8_t id) = 0;
    virtual rov::RovResult<std::array<std::int16_t, 6>> getAllPropellerBases() = 0;
    virtual rov::RovResult<std::array<std::int16_t, 6>> getAllPropellerOutputs() = 0;

    virtual rov::RovResult<void> enableStabilization() = 0;
    virtual rov::RovResult<void> disableStabilization() = 0;
    virtual rov::RovResult<void> enableHorizontalSynchronization() = 0;
    virtual rov::RovResult<void> disableHorizontalSynchronization() = 0;

    virtual rov::RovResult<rov::DypReading> readDyp() = 0;
    virtual rov::RovResult<rov::MpuRaw> readMpu() = 0;
    virtual rov::RovResult<rov::SensorSnapshot> getSensorSnapshot() = 0;
    virtual rov::RovResult<rov::Attitude> getAttitude() = 0;
    virtual rov::RovResult<rov::StabilizationStatus> getStabilization() = 0;

    virtual rov::RovResult<void> stop() = 0;
    virtual rov::RovResult<void> move() = 0;
    virtual rov::RovResult<void> stopVertical() = 0;
    virtual rov::RovResult<void> moveVertical() = 0;
    virtual rov::RovResult<void> stopHorizontal() = 0;
    virtual rov::RovResult<void> moveHorizontal() = 0;
};

} // namespace gw

#endif // GW_CORE_IROV_CONTROL_HPP
