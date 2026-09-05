#ifndef GW_CORE_ROV_CONTROL_ADAPTER_HPP
#define GW_CORE_ROV_CONTROL_ADAPTER_HPP

#include "core/irov_control.hpp"
#include "rov/rov_control.hpp"

namespace gw {

// Thin forwarder from the gateway's IRovControl seam onto the real
// rov::RovControl client (RovControl API v1, copied unmodified under
// vendor/rov_control). Signatures match one-to-one, so every method is a
// pure pass-through; error mapping to wire codes stays in GatewayCore
// (DECISIONS D-06).
//
// Board-only: the vendor transport uses POSIX termios and is compiled on
// Linux only; host tests run against FakeRovControl instead.
class RovControlAdapter final : public IRovControl
{
public:
    explicit RovControlAdapter(rov::RovControl& rov)
        : rov_(rov)
    {
    }

    rov::RovResult<void> setServo(std::uint8_t id, std::uint8_t angle) override
    {
        return rov_.setServo(id, angle);
    }
    rov::RovResult<void> setAllServos(std::uint8_t angle) override
    {
        return rov_.setAllServos(angle);
    }
    rov::RovResult<void> centerServo(std::uint8_t id) override
    {
        return rov_.centerServo(id);
    }
    rov::RovResult<void> centerAllServos() override
    {
        return rov_.centerAllServos();
    }
    rov::RovResult<std::uint8_t> getServo(std::uint8_t id) override
    {
        return rov_.getServo(id);
    }
    rov::RovResult<std::array<std::uint8_t, 10>> getAllServos() override
    {
        return rov_.getAllServos();
    }

    rov::RovResult<void> setVerticalBase(std::int16_t value) override
    {
        return rov_.setVerticalBase(value);
    }
    rov::RovResult<void> setVerticalPropeller(std::uint8_t id,
                                              std::int16_t value) override
    {
        return rov_.setVerticalPropeller(id, value);
    }
    rov::RovResult<void> setHorizontalBase(std::int16_t value) override
    {
        return rov_.setHorizontalBase(value);
    }
    rov::RovResult<void> setHorizontalPropeller(std::uint8_t id,
                                                std::int16_t value) override
    {
        return rov_.setHorizontalPropeller(id, value);
    }

    rov::RovResult<std::int16_t> getPropellerBase(std::uint8_t id) override
    {
        return rov_.getPropellerBase(id);
    }
    rov::RovResult<std::int16_t> getPropellerOutput(std::uint8_t id) override
    {
        return rov_.getPropellerOutput(id);
    }
    rov::RovResult<std::array<std::int16_t, 6>> getAllPropellerBases() override
    {
        return rov_.getAllPropellerBases();
    }
    rov::RovResult<std::array<std::int16_t, 6>> getAllPropellerOutputs() override
    {
        return rov_.getAllPropellerOutputs();
    }

    rov::RovResult<void> enableStabilization() override
    {
        return rov_.enableStabilization();
    }
    rov::RovResult<void> disableStabilization() override
    {
        return rov_.disableStabilization();
    }
    rov::RovResult<void> enableHorizontalSynchronization() override
    {
        return rov_.enableHorizontalSynchronization();
    }
    rov::RovResult<void> disableHorizontalSynchronization() override
    {
        return rov_.disableHorizontalSynchronization();
    }

    rov::RovResult<rov::DypReading> readDyp() override { return rov_.readDyp(); }
    rov::RovResult<rov::MpuRaw> readMpu() override { return rov_.readMpu(); }
    rov::RovResult<rov::SensorSnapshot> getSensorSnapshot() override
    {
        return rov_.getSensorSnapshot();
    }
    rov::RovResult<rov::Attitude> getAttitude() override
    {
        return rov_.getAttitude();
    }
    rov::RovResult<rov::StabilizationStatus> getStabilization() override
    {
        return rov_.getStabilization();
    }

    rov::RovResult<void> stop() override { return rov_.stop(); }
    rov::RovResult<void> move() override { return rov_.move(); }
    rov::RovResult<void> stopVertical() override { return rov_.stopVertical(); }
    rov::RovResult<void> moveVertical() override { return rov_.moveVertical(); }
    rov::RovResult<void> stopHorizontal() override
    {
        return rov_.stopHorizontal();
    }
    rov::RovResult<void> moveHorizontal() override
    {
        return rov_.moveHorizontal();
    }

private:
    rov::RovControl& rov_;
};

} // namespace gw

#endif // GW_CORE_ROV_CONTROL_ADAPTER_HPP
