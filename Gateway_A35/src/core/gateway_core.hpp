#ifndef GW_CORE_GATEWAY_CORE_HPP
#define GW_CORE_GATEWAY_CORE_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "core/command_queue.hpp"
#include "core/gateway_state.hpp"
#include "core/irov_control.hpp"
#include "wire/payload_codec.hpp"
#include "wire/wire_codec.hpp"

namespace gw {

// Tunables; an INI layer maps onto this in phase 5.
struct GatewayConfig
{
    // Safe-mode thruster clamp in percent (DECISIONS D-09: placeholder
    // +/-30 until boundary confirmation; 100 disables the clamp).
    int safeLimitPct = 30;

    // Emergency stop on client disconnect (D-07; requires user
    // confirmation before real-thruster tests).
    bool stopOnDisconnect = true;

    // Drop the client when no heartbeat (0x00F0) arrives within this
    // window; 0 disables the check (D-17). The Windows terminal sends
    // heartbeats every 1s by default.
    int heartbeatTimeoutMs = 5000;

    // Periodic full-state re-push interval.
    int statePeriodMs = 1000;

    // Bounded normal queue; urgent commands are never dropped.
    int normalQueueCapacity = 256;

    // Outbound frame queue cap (100 Hz summaries vs. a disconnected
    // client); oldest frames are dropped, newest-wins.
    std::size_t outboundQueueCapacity = 512U;
};

// Sensor-layer hooks invoked on Windows sensor queries (phase 3): the core
// stays decoupled from the sensor implementation while queries refresh the
// caches and push a fresh summary frame after the ACK.
class ISensorBridge
{
public:
    virtual ~ISensorBridge() = default;
    virtual void refreshMpu() = 0;
    virtual void refreshDyp() = 0;
    virtual void refreshSnapshot() = 0;
    virtual bool pushLatestSummary() = 0;
};

// One outbound frame (unencoded); the transport encodes with encodeFrame.
struct OutboundFrame
{
    std::uint16_t funcId = 0U;
    std::uint16_t seq = 0U;
    std::uint8_t flags = 0U;
    std::vector<std::uint8_t> payload;
};

// The protocol converter, state authority and command scheduler. Driven by
// one execution thread (the TCP server thread): submitRequest/drainQueue
// are the inbound path, takeOutboundFrames the outbound path. Sensor
// threads (phase 3) only append telemetry frames through pushTelemetry,
// which is mutex-guarded.
class GatewayCore
{
public:
    GatewayCore(IRovControl& rov, GatewayConfig config = GatewayConfig());

    // Optional sensor-layer hooks (phase 3); never owned, never null-deref'd.
    void setSensorBridge(ISensorBridge* bridge) { sensorBridge_ = bridge; }

    // Startup alignment per DECISIONS U-02/U-03: read stabilization
    // telemetry (he/gs/vs/hs), explicitly enable stabilization and
    // horizontal synchronization (idempotent on M33), set the A35-local
    // vertical synchronization bit. Returns false on a hard failure (caller
    // retries / refuses to serve).
    bool alignStartupState();

    const wire::StateV2& state() const { return state_.current(); }

    // ---- client lifecycle --------------------------------------------------
    void onClientConnected();    // pushes a full StateEventV2 immediately
    void onClientDisconnected(); // clears pending commands (no replay),
                                 // optional emergency stop (stopOnDisconnect)

    bool clientConnected() const { return clientConnected_; }

    // ---- inbound -----------------------------------------------------------
    // Heartbeats and unknown functions are handled inline (never queued);
    // everything else goes through the priority queue.
    void submitRequest(const wire::WireFrame& frame);

    // Executes queued commands by priority; maxOps=0 drains everything.
    // Returns the number of executed commands.
    std::size_t drainQueue(std::size_t maxOps = 0);

    // Periodic service (call at ~1Hz): re-pushes the full state.
    void tickPeriodic();

    // Heartbeat liveness (deadline-based; test hook below).
    bool heartbeatExpired() const;
    void setLastHeartbeatForTest(
            std::chrono::steady_clock::time_point t);

    // ---- outbound ----------------------------------------------------------
    std::vector<OutboundFrame> takeOutboundFrames();

    // Thread-safe telemetry injection for the 100Hz summary stream
    // (phase 3 sensor threads).
    void pushTelemetry(const OutboundFrame& frame);

    // Last-ditch emergency stop used on service shutdown.
    void shutdownStop();

private:
    void handleOne(const wire::WireFrame& frame);
    void executeStopAll(const wire::WireFrame& f, bool estop, bool emergency);
    void executeMoveAll(const wire::WireFrame& f);
    void executeGroupStopMove(const wire::WireFrame& f, bool vertical,
                              bool stop);
    void executeSafe(const wire::WireFrame& f, bool on);
    void executeStabilization(const wire::WireFrame& f, bool on);
    void executeVerticalSync(const wire::WireFrame& f, bool on);
    void executeHorizontalSync(const wire::WireFrame& f, bool on);
    void executeServoSet(const wire::WireFrame& f);
    void executeServoSetAll(const wire::WireFrame& f);
    void executeServoMid(const wire::WireFrame& f);
    void executeServoGet(const wire::WireFrame& f, bool all);
    void executePropellerSet(const wire::WireFrame& f);
    void executePropellerSetAll(const wire::WireFrame& f);
    void executePropellerStop(const wire::WireFrame& f);
    void executePropellerGet(const wire::WireFrame& f, bool base);
    void executeBaseValueVh(const wire::WireFrame& f);
    void executeSensorQuery(const wire::WireFrame& f);

    // Thruster translation (DECISIONS U-01 / phase-0 section 3.4):
    //  - vertical: stabilization ON -> setVerticalBase, OFF -> individual
    //  - horizontal: synchronization ON -> setHorizontalBase, OFF -> individual
    rov::RovResult<void> sendVertical(std::uint8_t id, std::int16_t value);
    rov::RovResult<void> sendHorizontal(std::uint8_t id, std::int16_t value);
    // Group form: one base call covers the group in base mode; individual
    // commands are expanded per channel otherwise (U-01: A35 assembles
    // vertical group semantics from multiple M33 commands).
    rov::RovResult<void> sendVerticalGroup(std::int16_t value);
    rov::RovResult<void> sendHorizontalGroup(std::int16_t value);

    std::int16_t clampSafe(std::int16_t valuePct) const;

    void updateState(const wire::StateV2& next);
    void ack(std::uint16_t seq, wire::ErrCode code);
    void pushStateIfChanged(bool forced = false);
    void pushAlarm(std::uint8_t level, std::uint16_t code, const char* text);

    static wire::ErrCode mapRovFailure(const rov::RovFailure& failure);

    IRovControl& rov_;
    GatewayConfig config_;
    GatewayState state_;
    CommandQueue queue_;
    ISensorBridge* sensorBridge_ = nullptr;
    bool clientConnected_ = false;
    std::chrono::steady_clock::time_point lastHeartbeat_;
    std::mutex outboundMutex_;
    std::deque<OutboundFrame> outbound_;
};

} // namespace gw

#endif // GW_CORE_GATEWAY_CORE_HPP
