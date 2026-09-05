#ifndef GW_SENSORS_SENSOR_SERVICE_HPP
#define GW_SENSORS_SENSOR_SERVICE_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "core/gateway_core.hpp"
#include "core/irov_control.hpp"
#include "sensors/dht11_reader.hpp"
#include "sensors/ina226_reader.hpp"
#include "sensors/m33_sensor_reader.hpp"
#include "sensors/sensor_types.hpp"
#include "wire/payload_codec.hpp"

namespace gw::sensors {

// M33 data-path watchdog decision input (T-01, user-approved 2026-09-04).
// Pure data + pure function so the policy is unit-testable without RPMsg.
struct MpuWatchdogState
{
    bool mpuEverSucceeded = false;
    std::int64_t msSinceStart = 0;      // since service construction
    std::int64_t msSinceMpuSuccess = 0; // meaningful when mpuEverSucceeded
};

// Fires when the service is past its grace window and the last MPU success
// is older than timeoutMs (or MPU never succeeded at all past grace).
// timeoutMs <= 0 disables the watchdog entirely.
bool mpuWatchdogFires(const MpuWatchdogState& state, int graceMs,
                      int timeoutMs);

// Owns all sensor acquisition and the 100Hz SensorSummary stream:
//   DHT11 thread     default 2 s (>= 1 s enforced)
//   INA226 thread    default 500 ms
//   MPU thread       default 20 ms (50 Hz over RovControl)
//   DYP thread       default 200 ms cadence + on-demand refresh flag
//   Summary thread   default 10 ms (100 Hz) -> FrameSink (GatewayCore)
//
// Windows sensor queries are served from the caches; the only M33 reads
// they may trigger are the fast MPU read and the cached snapshot, while a
// DYP query only raises a flag consumed by the DYP thread (the blocking
// measurement must never run on the TCP dispatch thread - phase 0
// section 8).
class SensorService : public ISensorBridge
{
public:
    using FrameSink = std::function<void(const OutboundFrame&)>;

    SensorService(IRovControl& rov, SensorConfig config, FrameSink sink);
    ~SensorService() override;

    SensorService(const SensorService&) = delete;
    SensorService& operator=(const SensorService&) = delete;

    // ---- threads (production); tests drive the poll API instead ----
    void start();
    void stop();

    // ---- manual acquisition (tests / external main loop) ----
    void pollDht11Once();
    void pollIna226Once();
    void pollMpuOnce();
    void pollDypOnce();

    // ---- cached snapshots (thread-safe) ----
    Dht11Sample dht11() const;
    Ina226Sample ina226() const;
    MpuSample mpu() const;
    DypSample dyp() const;

    int dht11Failures() const { return failures_[0].load(); }
    int ina226Failures() const { return failures_[1].load(); }
    int mpuFailures() const { return failures_[2].load(); }
    int dypFailures() const { return failures_[3].load(); }

    // M33 data-path health for the watchdog in the gateway main loop.
    MpuWatchdogState watchdogState() const;

    // ---- summary assembly ----
    // Freshness = last Ok sample within staleFactor * period. The `now`
    // parameter lets tests exercise staleness deterministically.
    wire::SensorSummaryData buildSummary(SensorTime now = SensorClock::now()) const;
    OutboundFrame buildSummaryFrame(SensorTime now = SensorClock::now()) const;

    // ---- ISensorBridge (called by GatewayCore on sensor queries) ----
    void refreshMpu() override;        // fast RPMsg round trip, inline
    void refreshDyp() override;        // documented no-op, see .cpp
    void refreshSnapshot() override;   // MPU freshness re-read, see .cpp
    bool pushLatestSummary() override; // enqueue one 0x0100 frame

private:
    void runPeriodic(int periodMs, const std::function<void()>& body);
    void summaryLoop();

    mutable std::mutex cacheMutex_;
    Dht11Sample dht11_;
    Ina226Sample ina226_;
    MpuSample mpu_;
    DypSample dyp_;
    // Watchdog bookkeeping: written under cacheMutex_ by the MPU poll thread,
    // read by watchdogState() from the supervision loop.
    SensorTime startedAt_{};
    SensorTime lastMpuSuccessAt_{};
    bool mpuEverSucceeded_ = false;

    std::atomic<int> failures_[4] = {}; // value-init: atomic default ctor
                                        // alone leaves the int uninitialized
    std::atomic<bool> running_{false};
    std::vector<std::thread> threads_;

    SensorConfig config_;
    FrameSink sink_;
    std::unique_ptr<Dht11Reader> dht11Reader_;
    std::unique_ptr<Ina226Reader> ina226Reader_;
    std::unique_ptr<M33SensorReader> m33Reader_;
};

} // namespace gw::sensors

#endif // GW_SENSORS_SENSOR_SERVICE_HPP
