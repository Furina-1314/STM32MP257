#include "sensors/sensor_service.hpp"

#include <chrono>
#include <utility>

#include "util/log.hpp"
#include "wire/function_registry.hpp"

namespace gw::sensors {

namespace {

std::int64_t msSince(SensorTime then, SensorTime now)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - then)
            .count();
}

std::uint32_t boardTimeMsNow()
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            SensorClock::now().time_since_epoch());
    return static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(ms.count()) & 0xFFFFFFFFULL);
}

// Stable per-sensor keys for the rate limiter (no pointer arithmetic).
const char kLogKeyDht11 = 0;
const char kLogKeyIna226 = 0;
const char kLogKeyMpu = 0;
const char kLogKeyDyp = 0;

} // namespace

bool mpuWatchdogFires(const MpuWatchdogState& state, int graceMs,
                      int timeoutMs)
{
    if (timeoutMs <= 0) {
        return false; // disabled
    }
    if (state.msSinceStart < static_cast<std::int64_t>(graceMs)) {
        return false; // startup grace: first poll + convergence headroom
    }
    if (!state.mpuEverSucceeded) {
        return true; // M33 data path never came up past grace
    }
    return state.msSinceMpuSuccess >= static_cast<std::int64_t>(timeoutMs);
}

SensorService::SensorService(IRovControl& rov, SensorConfig config,
                             FrameSink sink)
    : config_(config)
    , sink_(std::move(sink))
{
    startedAt_ = SensorClock::now();
    if (config_.dht11PeriodMs < 1000) {
        logMessage(LogLevel::Warning,
                    "sensor: dht11 period clamped to 1000 ms (datasheet floor)");
        config_.dht11PeriodMs = 1000;
    }
    if (config_.ina226PeriodMs < 50) {
        config_.ina226PeriodMs = 50;
    }
    if (config_.mpuPeriodMs < 10) {
        config_.mpuPeriodMs = 10;
    }
    if (config_.dypPeriodMs < 100) {
        config_.dypPeriodMs = 100; // trigger cadence + busy guard headroom
    }
    if (config_.summaryPeriodMs < 5) {
        config_.summaryPeriodMs = 5;
    }
    dht11Reader_ = std::make_unique<Dht11Reader>(config_);
    ina226Reader_ = std::make_unique<Ina226Reader>(config_);
    m33Reader_ = std::make_unique<M33SensorReader>(rov, config_);
}

SensorService::~SensorService()
{
    stop();
}

void SensorService::start()
{
    if (running_.exchange(true)) {
        return;
    }
    threads_.emplace_back([this] {
        runPeriodic(config_.dht11PeriodMs, [this] { pollDht11Once(); });
    });
    threads_.emplace_back([this] {
        runPeriodic(config_.ina226PeriodMs, [this] { pollIna226Once(); });
    });
    threads_.emplace_back([this] {
        runPeriodic(config_.mpuPeriodMs, [this] { pollMpuOnce(); });
    });
    threads_.emplace_back([this] {
        runPeriodic(config_.dypPeriodMs, [this] { pollDypOnce(); });
    });
    threads_.emplace_back([this] { summaryLoop(); });
}

void SensorService::stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    for (std::thread& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
}

void SensorService::runPeriodic(int periodMs,
                                const std::function<void()>& body)
{
    auto next = SensorClock::now();
    while (running_.load()) {
        body();
        next += std::chrono::milliseconds(periodMs);
        const auto now = SensorClock::now();
        if (next <= now) {
            next = now + std::chrono::milliseconds(periodMs); // overran
        }
        std::this_thread::sleep_until(next);
    }
}

void SensorService::summaryLoop()
{
    auto next = SensorClock::now();
    while (running_.load()) {
        pushLatestSummary();
        next += std::chrono::milliseconds(config_.summaryPeriodMs);
        const auto now = SensorClock::now();
        if (next <= now) {
            next = now + std::chrono::milliseconds(config_.summaryPeriodMs);
        }
        std::this_thread::sleep_until(next);
    }
}

// ---- acquisition -------------------------------------------------------------

void SensorService::pollDht11Once()
{
    Dht11Sample sample = dht11Reader_->poll();
    const bool ok = sample.ok();
    {
        const std::lock_guard<std::mutex> lock(cacheMutex_);
        dht11_ = sample;
    }
    if (ok) {
        failures_[0].store(0);
    } else {
        const int count = failures_[0].fetch_add(1) + 1; // saturates below
        if (count > 1000000) {
            return; // saturated: stop counting, stop logging
        }
        logRateLimited(&kLogKeyDht11,
                       ("sensor: dht11 failure x" + std::to_string(count)
                        + ": " + sample.errorMessage).c_str(),
                       10000);
    }
}

void SensorService::pollIna226Once()
{
    Ina226Sample sample = ina226Reader_->poll();
    const bool ok = sample.ok();
    {
        const std::lock_guard<std::mutex> lock(cacheMutex_);
        ina226_ = sample;
    }
    if (ok) {
        failures_[1].store(0);
    } else {
        const int count = failures_[1].fetch_add(1) + 1; // saturates below
        if (count > 1000000) {
            return; // saturated: stop counting, stop logging
        }
        logRateLimited(&kLogKeyIna226,
                       ("sensor: ina226 failure x" + std::to_string(count)
                        + ": " + sample.errorMessage).c_str(),
                       10000);
    }
}

void SensorService::pollMpuOnce()
{
    MpuSample sample = m33Reader_->pollMpu();
    const bool ok = sample.ok();
    {
        const std::lock_guard<std::mutex> lock(cacheMutex_);
        mpu_ = sample;
        if (ok) {
            lastMpuSuccessAt_ = sample.timestamp;
            mpuEverSucceeded_ = true;
        }
    }
    if (ok) {
        failures_[2].store(0);
    } else {
        // Rate-limited like DHT11/INA226: a wedged M33 data path (T-01) must
        // be visible in the journal, not silently zeroed via the validMask.
        const int count = failures_[2].fetch_add(1) + 1;
        if (count > 1000000) {
            return; // saturated: stop counting, stop logging
        }
        logRateLimited(&kLogKeyMpu,
                       ("sensor: mpu failure x" + std::to_string(count)
                        + ": " + sample.errorMessage).c_str(),
                       10000);
    }
}

void SensorService::pollDypOnce()
{
    DypSample sample = m33Reader_->pollDyp();
    const bool ok = sample.ok();
    {
        const std::lock_guard<std::mutex> lock(cacheMutex_);
        dyp_ = sample;
    }
    if (ok) {
        failures_[3].store(0);
    } else {
        const int count = failures_[3].fetch_add(1) + 1;
        // OutOfRange is the documented bench condition (65533 sentinel,
        // D-10): count it, but only M33-side errors are worth journal space.
        if ((count <= 1000000)
            && (sample.status == SensorStatus::DeviceError)) {
            logRateLimited(&kLogKeyDyp,
                           ("sensor: dyp failure x" + std::to_string(count)
                            + ": " + sample.errorMessage).c_str(),
                           10000);
        }
    }
}

// ---- caches -------------------------------------------------------------------

Dht11Sample SensorService::dht11() const
{
    const std::lock_guard<std::mutex> lock(cacheMutex_);
    return dht11_;
}

Ina226Sample SensorService::ina226() const
{
    const std::lock_guard<std::mutex> lock(cacheMutex_);
    return ina226_;
}

MpuSample SensorService::mpu() const
{
    const std::lock_guard<std::mutex> lock(cacheMutex_);
    return mpu_;
}

DypSample SensorService::dyp() const
{
    const std::lock_guard<std::mutex> lock(cacheMutex_);
    return dyp_;
}

MpuWatchdogState SensorService::watchdogState() const
{
    const std::lock_guard<std::mutex> lock(cacheMutex_);
    const SensorTime now = SensorClock::now();
    MpuWatchdogState state;
    state.mpuEverSucceeded = mpuEverSucceeded_;
    state.msSinceStart = msSince(startedAt_, now);
    state.msSinceMpuSuccess = mpuEverSucceeded_
            ? msSince(lastMpuSuccessAt_, now) : 0;
    return state;
}

// ---- summary ------------------------------------------------------------------

wire::SensorSummaryData
SensorService::buildSummary(SensorTime now) const
{
    Dht11Sample dht;
    Ina226Sample ina;
    MpuSample mpu;
    DypSample dyp;
    {
        const std::lock_guard<std::mutex> lock(cacheMutex_);
        dht = dht11_;
        ina = ina226_;
        mpu = mpu_;
        dyp = dyp_;
    }

    wire::SensorSummaryData data;
    data.boardTimeMs = boardTimeMsNow();

    const std::int64_t dhtStaleMs =
            static_cast<std::int64_t>(config_.staleFactor) * config_.dht11PeriodMs;
    const std::int64_t inaStaleMs =
            static_cast<std::int64_t>(config_.staleFactor) * config_.ina226PeriodMs;
    const std::int64_t mpuStaleMs =
            static_cast<std::int64_t>(config_.staleFactor) * config_.mpuPeriodMs;
    const std::int64_t dypStaleMs =
            static_cast<std::int64_t>(config_.staleFactor) * config_.dypPeriodMs;

    if (dht.ok() && (msSince(dht.timestamp, now) < dhtStaleMs)) {
        data.tempC = dht.temperatureC;
        data.humidPct = dht.humidityPercent;
        data.validMask = static_cast<std::uint8_t>(data.validMask
                                                   | wire::kValidTempHum);
    }
    if (mpu.ok() && (msSince(mpu.timestamp, now) < mpuStaleMs)) {
        for (int i = 0; i < 3; ++i) {
            data.accelMps2[i] = mpu.accelMps2[i];
            data.gyroRadS[i] = mpu.gyroRadS[i];
        }
        data.validMask =
                static_cast<std::uint8_t>(data.validMask | wire::kValidMpu);
    }
    if (ina.ok() && (msSince(ina.timestamp, now) < inaStaleMs)) {
        data.voltage = ina.busVoltageV;
        data.validMask = static_cast<std::uint8_t>(data.validMask
                                                   | wire::kValidVoltage);
    }
    if (dyp.valid && dyp.ok() && (msSince(dyp.timestamp, now) < dypStaleMs)) {
        data.distMm = dyp.distanceMm;
        data.validMask =
                static_cast<std::uint8_t>(data.validMask | wire::kValidDyp);
    } else {
        data.distMm = wire::kInvalidDistanceMm; // invalid DYP sentinel
    }
    return data;
}

OutboundFrame SensorService::buildSummaryFrame(SensorTime now) const
{
    wire::SensorSummaryData data = buildSummary(now);
    OutboundFrame frame;
    frame.funcId = static_cast<std::uint16_t>(wire::Func::SensorSummary);
    frame.seq = 0U;
    frame.flags = wire::kFlagEvent;
    frame.payload = wire::buildSensorSummary(data); // sanitizes non-finite
    return frame;
}

// ---- ISensorBridge --------------------------------------------------------------

void SensorService::refreshMpu()
{
    // No inline extra read: the MPU poll thread keeps the cache fresh at
    // mpuPeriodMs, and GatewayCore's own availability read already issued
    // one RPMsg round trip for the honest ACK. A second read here would
    // double the M33 traffic per query (board observation: repeated MPU
    // queries compound M33-side slowness).
}

void SensorService::refreshDyp()
{
    // Deliberately no inline measurement: a DYP read blocks up to the M33
    // 60 ms response window (plus busy states), which must never delay the
    // TCP dispatch thread (phase 0 section 8). The DYP thread polls
    // continuously at dypPeriodMs, so the cache served to the query is at
    // most one cycle old and the refreshed value reaches the 100 Hz stream
    // within that cycle.
}

void SensorService::refreshSnapshot()
{
    // The M33 snapshot has no field in the 45-byte summary; the periodic
    // poller owns freshness (see refreshMpu note on duplicate reads).
}

bool SensorService::pushLatestSummary()
{
    if (!sink_) {
        return false;
    }
    sink_(buildSummaryFrame());
    return true;
}

} // namespace gw::sensors
