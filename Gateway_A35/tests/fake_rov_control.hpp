#ifndef GW_TESTS_FAKE_ROV_CONTROL_HPP
#define GW_TESTS_FAKE_ROV_CONTROL_HPP

// Scriptable IRovControl double mirroring the M33 wire semantics that
// matter to the gateway (protocol v1.0 sections 5-9):
//   - servo commands are never gated by modes or stop latches;
//   - individual vertical (CH10-13) is rejected with err safety while
//     stabilization is on; individual horizontal (CH14-15) is rejected
//     while synchronization is on;
//   - base setters are legal in both modes but blocked by stop latches;
//   - base queries are mode-gated (vertical base requires stabilization,
//     horizontal base requires synchronization);
//   - stop/move family is never mode-gated (verified against the M33
//     source in phase 0, DECISIONS U-05);
//   - mode toggles are idempotent.
// Default boot state matches PropellerService_Init: stabilization on,
// synchronization on, latches clear.

#include "core/irov_control.hpp"

#include <map>
#include <string>
#include <vector>

namespace gw {

class FakeRovControl final : public IRovControl
{
public:
    std::vector<std::string> calls;

    // One-shot failure injection keyed by call name, e.g. "setVerticalBase".
    void failNext(const std::string& call, rov::RovError error)
    {
        failures_[call] = error;
    }

    // --- state inspection/mutation for test setup ---
    bool stabEnabled = true;
    bool syncEnabled = true;
    bool globalStopped = false;
    bool verticalStopped = false;
    bool horizontalStopped = false;
    std::array<std::uint8_t, 10> servoAngles{
            90U, 90U, 90U, 90U, 90U, 90U, 90U, 90U, 90U, 90U};
    std::int16_t verticalBase = 0;
    std::int16_t horizontalBase = 0;
    std::array<std::int16_t, 4> vertical{};
    std::array<std::int16_t, 2> horizontal{};
    std::uint16_t dypDistanceMm = 500U;
    bool dypBusy = false;
    rov::MpuRaw mpuRaw{}; // configurable raw sample served by readMpu

    std::size_t countCalls(const std::string& name) const
    {
        std::size_t n = 0U;
        for (const std::string& c : calls) {
            if (c == name) {
                ++n;
            }
        }
        return n;
    }

    bool called(const std::string& name) const
    {
        return countCalls(name) != 0U;
    }

    // --- helpers ---
    static rov::RovFailure safetyFailure(const char* detail)
    {
        rov::RovFailure failure;
        failure.code = rov::RovError::Safety;
        failure.origin = rov::ErrorOrigin::M33;
        failure.detail = detail;
        return failure;
    }

    rov::RovResult<void> okVoid(const std::string& call)
    {
        calls.push_back(call);
        const auto it = failures_.find(call);
        if (it != failures_.end()) {
            const rov::RovError error = it->second;
            failures_.erase(it);
            rov::RovFailure failure;
            failure.code = error;
            failure.origin = rov::ErrorOrigin::M33;
            return rov::RovResult<void>::fail(failure);
        }
        return rov::RovResult<void>::success();
    }

    // --- servo ---
    rov::RovResult<void> setServo(std::uint8_t id, std::uint8_t angle) override
    {
        servoAngles.at(id) = angle;
        return okVoid("setServo");
    }

    rov::RovResult<void> setAllServos(std::uint8_t angle) override
    {
        servoAngles.fill(angle);
        return okVoid("setAllServos");
    }

    rov::RovResult<void> centerServo(std::uint8_t id) override
    {
        servoAngles.at(id) = 90U;
        return okVoid("centerServo");
    }

    rov::RovResult<void> centerAllServos() override
    {
        servoAngles.fill(90U);
        return okVoid("centerAllServos");
    }

    rov::RovResult<std::uint8_t> getServo(std::uint8_t id) override
    {
        calls.push_back("getServo");
        return rov::RovResult<std::uint8_t>::success(servoAngles.at(id));
    }

    rov::RovResult<std::array<std::uint8_t, 10>> getAllServos() override
    {
        calls.push_back("getAllServos");
        return rov::RovResult<std::array<std::uint8_t, 10>>::success(
                servoAngles);
    }

    // --- propeller setters ---
    rov::RovResult<void> setVerticalBase(std::int16_t value) override
    {
        if (globalStopped || verticalStopped) {
            calls.push_back("setVerticalBase");
            return rov::RovResult<void>::fail(
                    safetyFailure("vertical latch"));
        }
        verticalBase = value;
        return okVoid("setVerticalBase");
    }

    rov::RovResult<void> setVerticalPropeller(std::uint8_t id,
                                              std::int16_t value) override
    {
        calls.push_back("setVerticalPropeller");
        if (stabEnabled) {
            return rov::RovResult<void>::fail(
                    safetyFailure("individual vertical under stabilization"));
        }
        if (globalStopped || verticalStopped) {
            return rov::RovResult<void>::fail(safetyFailure("vertical latch"));
        }
        vertical.at(static_cast<std::size_t>(id) - 10U) = value;
        return okVoidAlreadyLogged();
    }

    rov::RovResult<void> setHorizontalBase(std::int16_t value) override
    {
        if (globalStopped || horizontalStopped) {
            calls.push_back("setHorizontalBase");
            return rov::RovResult<void>::fail(
                    safetyFailure("horizontal latch"));
        }
        horizontalBase = value;
        return okVoid("setHorizontalBase");
    }

    rov::RovResult<void> setHorizontalPropeller(std::uint8_t id,
                                                std::int16_t value) override
    {
        calls.push_back("setHorizontalPropeller");
        if (syncEnabled) {
            return rov::RovResult<void>::fail(
                    safetyFailure("individual horizontal under sync"));
        }
        if (globalStopped || horizontalStopped) {
            return rov::RovResult<void>::fail(
                    safetyFailure("horizontal latch"));
        }
        horizontal.at(static_cast<std::size_t>(id) - 14U) = value;
        return okVoidAlreadyLogged();
    }

    // --- propeller queries ---
    rov::RovResult<std::int16_t> getPropellerBase(std::uint8_t id) override
    {
        calls.push_back("getPropellerBase");
        if (id <= 13U) {
            if (!stabEnabled) {
                return rov::RovResult<std::int16_t>::fail(
                        safetyFailure("vertical base needs stabilization"));
            }
            return rov::RovResult<std::int16_t>::success(verticalBase);
        }
        if (!syncEnabled) {
            return rov::RovResult<std::int16_t>::fail(
                    safetyFailure("horizontal base needs sync"));
        }
        return rov::RovResult<std::int16_t>::success(horizontalBase);
    }

    rov::RovResult<std::int16_t> getPropellerOutput(std::uint8_t id) override
    {
        calls.push_back("getPropellerOutput");
        if (id <= 13U) {
            return rov::RovResult<std::int16_t>::success(
                    stabEnabled ? verticalBase
                                : vertical.at(static_cast<std::size_t>(id)
                                              - 10U));
        }
        return rov::RovResult<std::int16_t>::success(
                syncEnabled ? horizontalBase
                            : horizontal.at(static_cast<std::size_t>(id)
                                            - 14U));
    }

    rov::RovResult<std::array<std::int16_t, 6>> getAllPropellerBases() override
    {
        calls.push_back("getAllPropellerBases");
        std::array<std::int16_t, 6> bases{};
        bases.fill(0);
        return rov::RovResult<std::array<std::int16_t, 6>>::success(bases);
    }

    rov::RovResult<std::array<std::int16_t, 6>> getAllPropellerOutputs()
            override
    {
        calls.push_back("getAllPropellerOutputs");
        std::array<std::int16_t, 6> outputs{};
        return rov::RovResult<std::array<std::int16_t, 6>>::success(outputs);
    }

    // --- modes (idempotent) ---
    rov::RovResult<void> enableStabilization() override
    {
        stabEnabled = true;
        return okVoid("enableStabilization");
    }

    rov::RovResult<void> disableStabilization() override
    {
        stabEnabled = false;
        return okVoid("disableStabilization");
    }

    rov::RovResult<void> enableHorizontalSynchronization() override
    {
        syncEnabled = true;
        return okVoid("enableHorizontalSynchronization");
    }

    rov::RovResult<void> disableHorizontalSynchronization() override
    {
        syncEnabled = false;
        return okVoid("disableHorizontalSynchronization");
    }

    // --- sensors ---
    rov::RovResult<rov::DypReading> readDyp() override
    {
        calls.push_back("readDyp");
        if (dypBusy) {
            rov::RovFailure failure;
            failure.code = rov::RovError::Busy;
            failure.origin = rov::ErrorOrigin::M33;
            return rov::RovResult<rov::DypReading>::fail(failure);
        }
        rov::DypReading reading;
        reading.distanceMm = dypDistanceMm;
        return rov::RovResult<rov::DypReading>::success(reading);
    }

    rov::RovResult<rov::MpuRaw> readMpu() override
    {
        calls.push_back("readMpu");
        const auto it = failures_.find("readMpu");
        if (it != failures_.end()) {
            const rov::RovError error = it->second;
            failures_.erase(it);
            rov::RovFailure failure;
            failure.code = error;
            failure.origin = rov::ErrorOrigin::M33;
            return rov::RovResult<rov::MpuRaw>::fail(failure);
        }
        return rov::RovResult<rov::MpuRaw>::success(mpuRaw);
    }

    rov::RovResult<rov::SensorSnapshot> getSensorSnapshot() override
    {
        calls.push_back("getSensorSnapshot");
        rov::SensorSnapshot snapshot;
        snapshot.mpuReady = true;
        snapshot.dypReady = true;
        snapshot.dypState = rov::DypState::Complete;
        snapshot.distanceMm = dypDistanceMm;
        return rov::RovResult<rov::SensorSnapshot>::success(snapshot);
    }

    rov::RovResult<rov::Attitude> getAttitude() override
    {
        calls.push_back("getAttitude");
        rov::Attitude attitude;
        attitude.ready = true;
        return rov::RovResult<rov::Attitude>::success(attitude);
    }

    rov::RovResult<rov::StabilizationStatus> getStabilization() override
    {
        calls.push_back("getStabilization");
        rov::StabilizationStatus status;
        status.horizontalEnabled = stabEnabled;
        status.globalStopped = globalStopped;
        status.verticalStopped = verticalStopped;
        status.horizontalStopped = horizontalStopped;
        status.attitudeReady = true;
        return rov::RovResult<rov::StabilizationStatus>::success(status);
    }

    // --- stop/move latches (never mode-gated, U-05) ---
    rov::RovResult<void> stop() override
    {
        globalStopped = true;
        vertical = {};
        horizontal = {};
        return okVoid("stop");
    }

    rov::RovResult<void> move() override
    {
        globalStopped = false;
        return okVoid("move");
    }

    rov::RovResult<void> stopVertical() override
    {
        verticalStopped = true;
        return okVoid("stopVertical");
    }

    rov::RovResult<void> moveVertical() override
    {
        verticalStopped = false;
        return okVoid("moveVertical");
    }

    rov::RovResult<void> stopHorizontal() override
    {
        horizontalStopped = true;
        return okVoid("stopHorizontal");
    }

    rov::RovResult<void> moveHorizontal() override
    {
        horizontalStopped = false;
        return okVoid("moveHorizontal");
    }

private:
    std::map<std::string, rov::RovError> failures_;

    rov::RovResult<void> okVoidAlreadyLogged()
    {
        const auto it = failures_.find(calls.back());
        if (it != failures_.end()) {
            const rov::RovError error = it->second;
            failures_.erase(it);
            rov::RovFailure failure;
            failure.code = error;
            failure.origin = rov::ErrorOrigin::M33;
            return rov::RovResult<void>::fail(failure);
        }
        return rov::RovResult<void>::success();
    }
};

} // namespace gw

#endif // GW_TESTS_FAKE_ROV_CONTROL_HPP
