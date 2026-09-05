// rov_gateway - board main (phase 4+).
//
// Owns the single RovControl instance (RPMsg single-reader/single-owner),
// the authoritative core, the sensor service and the TCP server. Shutdown
// on SIGINT/SIGTERM performs a best-effort global stop; the latch stays
// set by design (the next owner must explicitly move()).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include "core/gateway_core.hpp"
#include "core/rov_control_adapter.hpp"
#include "net/tcp_server.hpp"
#include "rov/rov_control.hpp"
#include "sensors/sensor_service.hpp"
#include "util/ini_config.hpp"
#include "util/log.hpp"
#include "wire/function_registry.hpp"

namespace {

std::atomic<bool> g_stopRequested{false};

void handleSignal(int)
{
    g_stopRequested.store(true);
}

using Clock = std::chrono::steady_clock;

// Dedicated exit code for the M33 data-path watchdog (T-01): systemd
// Restart=on-failure re-runs the start chain, whose m33 gate restarts a
// wedged M33 and re-latches the global stop via the self-test.
constexpr int kWatchdogExitCode = 90;

std::int64_t elapsedMs(Clock::time_point from)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   Clock::now() - from)
            .count();
}

gw::LogLevel logLevelFromText(const std::string& text)
{
    if (text == "debug") {
        return gw::LogLevel::Debug;
    }
    if (text == "warning") {
        return gw::LogLevel::Warning;
    }
    if (text == "error") {
        return gw::LogLevel::Error;
    }
    return gw::LogLevel::Info;
}

} // namespace

int main(int argc, char** argv)
{
    using namespace gw;

    const char* configPath = "/etc/rov_gateway.ini";
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "--config") == 0) && (i + 1 < argc)) {
            configPath = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: rov_gateway [--config /etc/rov_gateway.ini]\n");
            return 0;
        }
    }

    IniConfig ini;
    if (ini.load(configPath)) {
        logMessage(LogLevel::Info,
                    "gateway: config loaded from " + std::string(configPath)
                            + " (" + std::to_string(ini.size()) + " keys)");
    } else {
        logMessage(LogLevel::Warning,
                    "gateway: config " + std::string(configPath)
                            + " not readable, using built-in defaults");
    }
    setLogLevel(logLevelFromText(ini.get("log", "level", "info")));

    const std::string device = ini.get("rov", "device", "/dev/ttyRPMSG0");
    GatewayConfig coreConfig;
    coreConfig.safeLimitPct =
            ini.getInt("gateway", "safe_limit_pct", coreConfig.safeLimitPct);
    coreConfig.stopOnDisconnect = ini.getBool(
            "gateway", "stop_on_disconnect", coreConfig.stopOnDisconnect);
    coreConfig.heartbeatTimeoutMs = ini.getInt(
            "gateway", "heartbeat_timeout_ms", coreConfig.heartbeatTimeoutMs);
    coreConfig.statePeriodMs =
            ini.getInt("gateway", "state_period_ms", coreConfig.statePeriodMs);
    coreConfig.normalQueueCapacity = ini.getInt(
            "gateway", "normal_queue_capacity", coreConfig.normalQueueCapacity);
    coreConfig.outboundQueueCapacity = static_cast<std::size_t>(ini.getInt(
            "gateway", "outbound_queue_capacity",
            static_cast<int>(coreConfig.outboundQueueCapacity)));
    // M33 data-path watchdog (T-01, user-approved 2026-09-04). Auto-disabled
    // when MPU polling is slower than half the window: the criterion would
    // otherwise fire on healthy slow polls.
    const int watchdogTimeoutMs =
            ini.getInt("gateway", "mpu_watchdog_timeout_ms", 60000);
    const int watchdogGraceMs =
            ini.getInt("gateway", "mpu_watchdog_grace_ms", 30000);

    sensors::SensorConfig sensorConfig;
    sensorConfig.sysfsRoot = ini.get("sensors", "sysfs_root", "/sys");
    const std::string dht11Mode =
            ini.get("sensors", "dht11_mode", "auto");
    sensorConfig.dht11Mode =
            (dht11Mode == "alientek_misc") ? sensors::Dht11Mode::AlientekMisc
            : (dht11Mode == "standard_iio") ? sensors::Dht11Mode::StandardIio
                                            : sensors::Dht11Mode::Auto;
    sensorConfig.dht11PeriodMs =
            ini.getInt("sensors", "dht11_period_ms", sensorConfig.dht11PeriodMs);
    sensorConfig.dht11ThreeDigitCompat = ini.getBool(
            "sensors", "dht11_three_digit_compat",
            sensorConfig.dht11ThreeDigitCompat);
    sensorConfig.ina226PeriodMs = ini.getInt("sensors", "ina226_period_ms",
                                             sensorConfig.ina226PeriodMs);
    sensorConfig.mpuPeriodMs =
            ini.getInt("sensors", "mpu_period_ms", sensorConfig.mpuPeriodMs);
    sensorConfig.dypPeriodMs =
            ini.getInt("sensors", "dyp_period_ms", sensorConfig.dypPeriodMs);
    sensorConfig.staleFactor =
            ini.getInt("sensors", "stale_factor", sensorConfig.staleFactor);
    sensorConfig.dypValidMinMm = static_cast<float>(
            ini.getInt("sensors", "dyp_valid_min_mm", 20));
    sensorConfig.dypValidMaxMm = static_cast<float>(
            ini.getInt("sensors", "dyp_valid_max_mm", 8000));
    sensorConfig.busVoltageMinV = static_cast<float>(
            ini.getInt("sensors", "bus_voltage_min_v", 0));
    sensorConfig.busVoltageMaxV = static_cast<float>(
            ini.getInt("sensors", "bus_voltage_max_v", 36));
    sensorConfig.summaryPeriodMs = ini.getInt("sensors", "summary_period_ms",
                                              sensorConfig.summaryPeriodMs);

    TcpServerConfig serverConfig;
    serverConfig.port = static_cast<std::uint16_t>(
            ini.getInt("tcp", "port", serverConfig.port));
    serverConfig.bindAddr = ini.get("tcp", "bind", "0.0.0.0");
    const std::string policy = ini.get("tcp", "client_policy", "takeover");
    serverConfig.policy = (policy == "reject") ? ClientPolicy::RejectNew
                                               : ClientPolicy::Takeover;
    serverConfig.maxPayload =
            ini.getInt("tcp", "max_payload", serverConfig.maxPayload);
    serverConfig.recvBufferLimit = ini.getInt("tcp", "recv_buffer_limit",
                                              serverConfig.recvBufferLimit);

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    // ---- M33 client (the only owner of the RPMsg endpoint) ---------------
    rov::RovControl rov(device);
    const auto opened = rov.open();
    if (!opened) {
        logMessage(LogLevel::Error,
                    "gateway: cannot open " + device + ": "
                            + opened.failure.detail);
        return 1;
    }
    logMessage(LogLevel::Info,
                "gateway: RPMsg opened on " + device);

    RovControlAdapter adapter(rov);
    GatewayCore core(adapter, coreConfig);

    // Startup alignment (U-02/U-03) with bounded retry.
    bool aligned = false;
    for (int attempt = 1; (attempt <= 5) && !aligned && !g_stopRequested.load();
         ++attempt) {
        aligned = core.alignStartupState();
        if (!aligned) {
            logMessage(LogLevel::Warning,
                        "gateway: startup alignment failed (attempt "
                                + std::to_string(attempt) + "), retrying");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    if (!aligned) {
        logMessage(LogLevel::Error, "gateway: alignment failed, exiting");
        rov.close();
        return 1;
    }
    logMessage(LogLevel::Info, "gateway: startup alignment complete");

    // ---- sensor service + 100Hz summary stream ---------------------------
    sensors::SensorService::FrameSink summarySink =
            [&core](const OutboundFrame& f) { core.pushTelemetry(f); };
    sensors::SensorService sensorService(adapter, sensorConfig, summarySink);
    core.setSensorBridge(&sensorService);
    sensorService.start();

    // ---- TCP server -------------------------------------------------------
    int connects = 0;
    int disconnects = 0;
    TcpServer server(serverConfig,
                     [&core](const wire::WireFrame& f) {
                         core.submitRequest(f);
                         core.drainQueue(); // low-latency in-thread dispatch
                     },
                     [&core, &connects] {
                         ++connects;
                         core.onClientConnected();
                     },
                     [&core, &disconnects] {
                         ++disconnects;
                         core.onClientDisconnected();
                     });
    if (!server.start()) {
        logMessage(LogLevel::Error,
                    "gateway: cannot listen on port "
                            + std::to_string(serverConfig.port));
        sensorService.stop();
        rov.close();
        return 1;
    }
    std::thread serverThread([&server] { server.run(); });
    logMessage(LogLevel::Info,
                "gateway: listening on " + serverConfig.bindAddr + ":"
                        + std::to_string(serverConfig.port));

    // ---- main supervision loop: outbound flush, 1Hz state, heartbeat -----
    auto lastStateTick = Clock::now();
    auto lastWatchdogTick = Clock::now();
    bool watchdogEnabled = (watchdogTimeoutMs > 0)
            && (sensorConfig.mpuPeriodMs < watchdogTimeoutMs / 2);
    while (!g_stopRequested.load()) {
        const auto frames = core.takeOutboundFrames();
        if (!frames.empty()) {
            server.send(frames);
        }

        if (elapsedMs(lastStateTick) >= 1000) {
            lastStateTick = Clock::now();
            core.tickPeriodic(); // 1Hz full-state re-push
        }

        if (watchdogEnabled && (elapsedMs(lastWatchdogTick) >= 5000)) {
            lastWatchdogTick = Clock::now();
            const sensors::MpuWatchdogState health =
                    sensorService.watchdogState();
            if (sensors::mpuWatchdogFires(health, watchdogGraceMs,
                                          watchdogTimeoutMs)) {
                logMessage(LogLevel::Error,
                            "gateway: M33 data-path watchdog fired (mpu dead "
                            + std::to_string(health.mpuEverSucceeded
                                     ? health.msSinceMpuSuccess
                                     : health.msSinceStart)
                            + " ms); exiting for M33 recovery");
                // Fall through the normal shutdown path: best-effort stop
                // against the (likely wedged) M33, latch preserved by the
                // self-test that runs on the systemd restart.
                break;
            }
        }

        if (server.hasClient() && core.heartbeatExpired()) {
            logMessage(LogLevel::Warning,
                        "gateway: heartbeat timeout, dropping client");
            server.dropClient();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const bool watchdogExit = !g_stopRequested.load();

    // ---- graceful shutdown -------------------------------------------------
    logMessage(LogLevel::Info, "gateway: shutting down");
    server.stop();
    if (serverThread.joinable()) {
        serverThread.join();
    }
    core.onClientDisconnected(); // clear pending, latch policy, no frames
    core.shutdownStop();         // best-effort global stop; latch stays set
    sensorService.stop();
    rov.close();
    logMessage(LogLevel::Info,
                "gateway: stopped (global stop latched; move() required to "
                "re-enable)");
    return watchdogExit ? kWatchdogExitCode : 0;
}
