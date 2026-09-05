// SensorService end-to-end on a virtual sysfs + Fake RovControl: cache
// freshness, validMask assembly, MPU unit conversion, DYP validity policy,
// staleness degradation, failure counters, summary frame layout, and the
// GatewayCore sensor-query bridge.
#include "fake_rov_control.hpp"
#include "test_support.hpp"
#include "virtual_sysfs.hpp"

#include "core/gateway_core.hpp"
#include "sensors/sensor_service.hpp"
#include "util/log.hpp"
#include "wire/function_registry.hpp"

#include <chrono>
#include <cmath>
#include <mutex>

namespace {

using namespace std::chrono;

struct ServiceHarness
{
    gw::VirtualSysfs fs;
    gw::FakeRovControl rov;
    gw::sensors::SensorConfig config;
    std::vector<gw::OutboundFrame> frames;
    std::mutex framesMutex;
    std::unique_ptr<gw::sensors::SensorService> service;

    ServiceHarness()
    {
        config.sysfsRoot = fs.root();
        // Fast, deterministic periods for staleness math; dht floor is
        // clamped to 1000 ms by the service.
        config.dht11PeriodMs = 1000;
        config.ina226PeriodMs = 500;
        config.mpuPeriodMs = 20;
        config.dypPeriodMs = 200;
        service = std::make_unique<gw::sensors::SensorService>(
                rov, config,
                [this](const gw::OutboundFrame& f) {
                    const std::lock_guard<std::mutex> lock(framesMutex);
                    frames.push_back(f);
                });
    }

    void populateAll()
    {
        fs.writeFile("class/misc/dht11/value", "6929");
        fs.writeFile("class/hwmon/hwmon1/name", "ina226\n");
        fs.writeFile("class/hwmon/hwmon1/in1_input", "16000");
        rov.mpuRaw = rov::MpuRaw{16384, -16384, 0, 131, -262, 0};
        rov.dypDistanceMm = 500U;
        service->pollDht11Once();
        service->pollIna226Once();
        service->pollMpuOnce();
        service->pollDypOnce();
    }
};

} // namespace

int main()
{
    using namespace gw;
    using wire::Func;
    using SC = sensors::SensorStatus;
    setLogSink([](LogLevel, const char*) {});
    setLogLevel(LogLevel::Error);

    // ---- happy path: all sensors fresh, validMask complete ---------------
    {
        ServiceHarness h;
        h.populateAll();

        const auto data = h.service->buildSummary();
        CHECK_EQ(data.validMask,
                 static_cast<std::uint8_t>(wire::kValidTempHum | wire::kValidMpu
                                           | wire::kValidVoltage
                                           | wire::kValidDyp));
        CHECK_EQ(static_cast<int>(data.tempC), 29);
        CHECK_EQ(static_cast<int>(data.humidPct), 69);
        CHECK(std::fabs(data.voltage - 16.0F) < 0.001F);
        CHECK(std::fabs(data.distMm - 500.0F) < 0.001F);

        // MPU conversion: raw 16384 LSB -> 1 g -> 9.80665 m/s^2;
        // raw 131 LSB -> 1 deg/s -> pi/180 rad/s.
        CHECK(std::fabs(data.accelMps2[0] - 9.80665F) < 0.0005F);
        CHECK(std::fabs(data.accelMps2[1] + 9.80665F) < 0.0005F);
        CHECK(std::fabs(data.accelMps2[2] - 0.0F) < 0.0005F);
        CHECK(std::fabs(data.gyroRadS[0] - 0.0174533F) < 0.00005F);
        CHECK(std::fabs(data.gyroRadS[1] + 0.0349066F) < 0.00005F);

        // Summary frame: 45-byte wire layout, event flags.
        const auto frame = h.service->buildSummaryFrame();
        CHECK_EQ(frame.funcId, static_cast<std::uint16_t>(Func::SensorSummary));
        CHECK_EQ(frame.flags, wire::kFlagEvent);
        CHECK_EQ(frame.payload.size(), static_cast<std::size_t>(45U));
        CHECK_EQ(frame.payload[40], data.validMask);
    }

    // ---- staleness: different windows degrade independently --------------
    {
        ServiceHarness h;
        h.populateAll();
        const auto t0 = sensors::SensorClock::now();

        // +1 s: mpu (60 ms) and dyp (600 ms) stale; dht (3 s) and
        // ina (1.5 s) still fresh.
        auto data = h.service->buildSummary(t0 + milliseconds(1000));
        CHECK_EQ(data.validMask,
                 static_cast<std::uint8_t>(wire::kValidTempHum
                                           | wire::kValidVoltage));
        CHECK_EQ(data.distMm, wire::kInvalidDistanceMm); // invalid sentinel

        // +10 s: everything stale, values zeroed except the sentinel.
        data = h.service->buildSummary(t0 + milliseconds(10000));
        CHECK_EQ(data.validMask, static_cast<std::uint8_t>(0U));
        CHECK_EQ(data.tempC, 0.0F);
        CHECK_EQ(data.distMm, wire::kInvalidDistanceMm);
    }

    // ---- DYP validity policy (D-10) ---------------------------------------
    {
        ServiceHarness h;
        h.rov.dypDistanceMm = 65533U; // UART-valid, physically implausible
        h.service->pollDypOnce();
        sensors::DypSample s = h.service->dyp();
        CHECK(s.status == SC::OutOfRange);
        CHECK(!s.valid);
        auto data = h.service->buildSummary();
        CHECK_EQ(static_cast<std::uint16_t>(data.validMask & wire::kValidDyp),
                 static_cast<std::uint16_t>(0U));
        CHECK_EQ(data.distMm, wire::kInvalidDistanceMm);

        h.rov.dypBusy = true;
        h.service->pollDypOnce();
        s = h.service->dyp();
        CHECK(s.status == SC::DeviceError);
        CHECK(!s.valid);

        h.rov.dypBusy = false;
        h.rov.dypDistanceMm = 20U; // window boundary inclusive
        h.service->pollDypOnce();
        s = h.service->dyp();
        CHECK(s.ok() && s.valid);
        h.rov.dypDistanceMm = 19U;
        h.service->pollDypOnce();
        CHECK(!h.service->dyp().valid);
    }

    // ---- failure counters and recovery ------------------------------------
    {
        ServiceHarness h;
        // DHT node missing: three failures, then recovery.
        h.service->pollDht11Once();
        h.service->pollDht11Once();
        h.service->pollDht11Once();
        CHECK_EQ(h.service->dht11Failures(), 3);
        CHECK(h.service->dht11().status == SC::NotFound);

        h.fs.writeFile("class/misc/dht11/value", "5021");
        h.service->pollDht11Once();
        CHECK_EQ(h.service->dht11Failures(), 0);
        CHECK(h.service->dht11().ok());

        // Fault isolation: with DHT failing again the rest still summarizes.
        h.populateAll();
        h.fs.removeFile("class/misc/dht11/value");
        h.service->pollDht11Once(); // fails, other caches intact
        auto data = h.service->buildSummary();
        CHECK_EQ(static_cast<std::uint16_t>(data.validMask
                                            & wire::kValidTempHum),
                 static_cast<std::uint16_t>(0U));
        CHECK_EQ(static_cast<std::uint16_t>(data.validMask & wire::kValidMpu),
                 wire::kValidMpu);
    }

    // ---- thread smoke: start/stop cycles cleanly --------------------------
    {
        ServiceHarness h;
        h.fs.writeFile("class/misc/dht11/value", "5525");
        h.service->start();
        std::this_thread::sleep_for(milliseconds(300));
        h.service->stop();
        h.service->stop(); // idempotent
        CHECK(!h.frames.empty());
        bool sawSummary = false;
        for (const auto& f : h.frames) {
            if (f.funcId == static_cast<std::uint16_t>(Func::SensorSummary)) {
                sawSummary = true;
                CHECK_EQ(f.payload.size(), static_cast<std::size_t>(45U));
            }
        }
        CHECK(sawSummary);
    }

    // ---- GatewayCore bridge: sensor queries push a fresh summary ----------
    {
        ServiceHarness h;
        h.populateAll();

        GatewayCore core(h.rov);
        CHECK(core.alignStartupState());
        core.setSensorBridge(h.service.get());
        core.onClientConnected();
        core.takeOutboundFrames();
        h.frames.clear();

        wire::WireFrame query;
        query.funcId = static_cast<std::uint16_t>(Func::SensorAll);
        query.seq = 77U;
        query.flags = wire::kFlagNeedAck;
        core.submitRequest(query);
        core.drainQueue();

        auto out = core.takeOutboundFrames();
        int acks = 0;
        for (const auto& f : out) {
            if (f.funcId == static_cast<std::uint16_t>(Func::Ack)
                && (f.seq == 77U)) {
                ++acks;
            }
        }
        CHECK_EQ(acks, 1);
        // The bridge pushes summaries through the service sink (h.frames),
        // not the core outbound queue.
        int summaries = 0;
        for (const auto& f : h.frames) {
            if (f.funcId == static_cast<std::uint16_t>(Func::SensorSummary)) {
                ++summaries;
                CHECK_EQ(f.payload.size(), static_cast<std::size_t>(45U));
            }
        }
        CHECK_EQ(summaries, 1);

        // MPU query: exactly one readMpu (core availability read; the
        // bridge refresh no longer duplicates it - see D-28).
        const std::size_t before = h.rov.countCalls("readMpu");
        query.funcId = static_cast<std::uint16_t>(Func::SensorMpu);
        query.seq = 78U;
        core.submitRequest(query);
        core.drainQueue();
        core.takeOutboundFrames();
        CHECK(h.rov.countCalls("readMpu") == before + 1U);
    }

    // ---- MPU watchdog (T-01, pure decision + state tracking) --------------
    {
        using gw::sensors::MpuWatchdogState;
        using gw::sensors::mpuWatchdogFires;
        MpuWatchdogState s;
        // Disabled
        CHECK(!mpuWatchdogFires(s, 30000, 0));
        // Grace window: never fires before grace regardless of MPU state
        s.msSinceStart = 1000;
        CHECK(!mpuWatchdogFires(s, 30000, 60000));
        // Past grace, MPU never succeeded -> fires
        s.msSinceStart = 31000;
        CHECK(mpuWatchdogFires(s, 30000, 60000));
        // Past grace, recent success -> healthy
        s.mpuEverSucceeded = true;
        s.msSinceMpuSuccess = 5000;
        CHECK(!mpuWatchdogFires(s, 30000, 60000));
        // Past grace, success older than timeout -> fires
        s.msSinceMpuSuccess = 61000;
        CHECK(mpuWatchdogFires(s, 30000, 60000));
        // Boundary: exactly the timeout still counts as failed
        s.msSinceMpuSuccess = 60000;
        CHECK(mpuWatchdogFires(s, 30000, 60000));

        // Service-level tracking: success marks everSucceeded and is sticky
        // across later failures.
        ServiceHarness h;
        h.service->pollMpuOnce();
        CHECK(h.service->watchdogState().mpuEverSucceeded);
        h.rov.failNext("readMpu", rov::RovError::Timeout);
        h.service->pollMpuOnce();
        CHECK(h.service->mpuFailures() == 1);
        const auto state = h.service->watchdogState();
        CHECK(state.mpuEverSucceeded);
        CHECK(state.msSinceStart >= 0);
        CHECK(state.msSinceMpuSuccess >= 0);
    }

    TEST_MAIN_END;
}
