#include "core/gateway_core.hpp"

#include <utility>

#include "wire/function_registry.hpp"

namespace gw {

using wire::ErrCode;

GatewayCore::GatewayCore(IRovControl& rov, GatewayConfig config)
    : rov_(rov)
    , config_(config)
{
}

// ---- startup alignment ----------------------------------------------------

bool GatewayCore::alignStartupState()
{
    const auto telemetry = rov_.getStabilization();
    if (!telemetry) {
        return false;
    }

    wire::StateV2 next = state_.current();
    next.attitudeStab = telemetry.value->horizontalEnabled;
    next.globalStopped = telemetry.value->globalStopped;
    next.verticalStopped = telemetry.value->verticalStopped;
    next.horizontalStopped = telemetry.value->horizontalStopped;

    // U-03: stabilization explicitly ON once (idempotent on M33, which
    // boots with horizontal on - belt and braces for a known state).
    if (!rov_.enableStabilization()) {
        return false;
    }
    next.attitudeStab = true;

    // U-02: horizontal synchronization ON (real M33 command, idempotent);
    // vertical synchronization is A35-local.
    if (!rov_.enableHorizontalSynchronization()) {
        return false;
    }
    next.horizontalSync = true;
    next.verticalSync = true;

    next.safe = false;
    next.estop = false;
    next.emergency = false;

    updateState(next);
    state_.forcePush(); // first client gets a full push regardless
    return true;
}

// ---- client lifecycle -------------------------------------------------------

void GatewayCore::onClientConnected()
{
    clientConnected_ = true;
    lastHeartbeat_ = std::chrono::steady_clock::now();
    state_.forcePush();
    pushStateIfChanged(true); // full authoritative state immediately
}

void GatewayCore::onClientDisconnected()
{
    clientConnected_ = false;
    queue_.clear(); // pending commands are dropped, never replayed
    if (config_.stopOnDisconnect) {
        const auto stopped = rov_.stop();
        wire::StateV2 next = state_.current();
        if (stopped) {
            next.globalStopped = true;
        }
        updateState(next);
    }
}

bool GatewayCore::heartbeatExpired() const
{
    if (!clientConnected_ || (config_.heartbeatTimeoutMs <= 0)) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastHeartbeat_);
    return elapsed.count() >= static_cast<std::int64_t>(config_.heartbeatTimeoutMs);
}

void GatewayCore::setLastHeartbeatForTest(std::chrono::steady_clock::time_point t)
{
    lastHeartbeat_ = t;
}

// ---- inbound ----------------------------------------------------------------

void GatewayCore::submitRequest(const wire::WireFrame& frame)
{
    if (static_cast<wire::Func>(frame.funcId) == wire::Func::Heartbeat) {
        std::uint32_t clientMs = 0U;
        if (wire::parseHeartbeat(frame.payload, clientMs)) {
            lastHeartbeat_ = std::chrono::steady_clock::now();
        }
        return; // silent by protocol; liveness only
    }

    const wire::FunctionEntry* entry = wire::findFunc(frame.funcId);
    if ((entry == nullptr) || (entry->direction != wire::Direction::Request)) {
        // Unknown function: never crash, answer unsupported when an ACK was
        // requested (protocol section 1 / VibePrompt section 4).
        if ((frame.flags & wire::kFlagNeedAck) != 0U) {
            ack(frame.seq, ErrCode::Unsupported);
        }
        return;
    }

    queue_.push(frame, entry->priority, config_.normalQueueCapacity);
}

std::size_t GatewayCore::drainQueue(std::size_t maxOps)
{
    std::size_t executed = 0;
    wire::WireFrame frame;
    while (queue_.pop(frame)) {
        handleOne(frame);
        ++executed;
        if ((maxOps != 0U) && (executed >= maxOps)) {
            break;
        }
    }
    return executed;
}

void GatewayCore::tickPeriodic()
{
    pushStateIfChanged(true); // 1Hz full-state re-push
}

void GatewayCore::shutdownStop()
{
    rov_.stop(); // best-effort; latch stays set after exit by design
    wire::StateV2 next = state_.current();
    next.globalStopped = true;
    updateState(next);
}

// ---- per-function execution -------------------------------------------------

void GatewayCore::handleOne(const wire::WireFrame& f)
{
    using wire::Func;
    switch (static_cast<Func>(f.funcId)) {
    case Func::Ask:
    case Func::Ver:
    case Func::Help:
        ack(f.seq, ErrCode::Ok); // A35-local answers, no data payload (D-14)
        break;
    case Func::Status:
        ack(f.seq, ErrCode::Ok);
        pushStateIfChanged(true);
        break;
    case Func::StopAll:
        executeStopAll(f, false, false);
        break;
    case Func::Emergency:
        executeStopAll(f, false, true);
        break;
    case Func::Estop:
        executeStopAll(f, true, false);
        break;
    case Func::MoveAll:
        executeMoveAll(f);
        break;
    case Func::StopVertical:
        executeGroupStopMove(f, true, true);
        break;
    case Func::MoveVertical:
        executeGroupStopMove(f, true, false);
        break;
    case Func::StopHorizontal:
        executeGroupStopMove(f, false, true);
        break;
    case Func::MoveHorizontal:
        executeGroupStopMove(f, false, false);
        break;
    case Func::SafeOn:
        executeSafe(f, true);
        break;
    case Func::SafeOff:
        executeSafe(f, false);
        break;
    case Func::HorizontalOn:
        executeStabilization(f, true);
        break;
    case Func::HorizontalOff:
        executeStabilization(f, false);
        break;
    case Func::VerticalSyncOn:
        executeVerticalSync(f, true);
        break;
    case Func::VerticalSyncOff:
        executeVerticalSync(f, false);
        break;
    case Func::HorizontalSyncOn:
        executeHorizontalSync(f, true);
        break;
    case Func::HorizontalSyncOff:
        executeHorizontalSync(f, false);
        break;
    case Func::ServoSet:
        executeServoSet(f);
        break;
    case Func::ServoSetAll:
        executeServoSetAll(f);
        break;
    case Func::ServoMid:
        executeServoMid(f);
        break;
    case Func::ServoGet:
        executeServoGet(f, false);
        break;
    case Func::ServoGetAll:
        executeServoGet(f, true);
        break;
    case Func::PropellerSet:
        executePropellerSet(f);
        break;
    case Func::PropellerSetAll:
        executePropellerSetAll(f);
        break;
    case Func::PropellerStop:
        executePropellerStop(f);
        break;
    case Func::PropellerGetBase:
        executePropellerGet(f, true);
        break;
    case Func::PropellerGetReal:
        executePropellerGet(f, false);
        break;
    case Func::BaseValue:
        ack(f.seq, ErrCode::Unsupported); // deprecated, never used
        break;
    case Func::BaseValueVH:
        executeBaseValueVh(f);
        break;
    case Func::SensorMpu:
    case Func::SensorDyp:
    case Func::SensorAll:
        executeSensorQuery(f);
        break;
    case Func::Heartbeat:
    case Func::SensorSummary:
    case Func::Ack:
    case Func::StateEvent:
    case Func::AlarmEvent:
    case Func::StateEventV2:
        // Wrong-direction frames are silently ignored (they are our own
        // outputs); nothing here may crash on them.
        break;
    }
}

// Stop/Estop/Emergency share one execution: M33 global stop zeroes the six
// thruster channels and latches; no servo command is ever issued. The three
// differ only in which state bits they set.
void GatewayCore::executeStopAll(const wire::WireFrame& f, bool estop,
                                 bool emergency)
{
    const auto result = rov_.stop();
    if (!result) {
        ack(f.seq, mapRovFailure(result.failure));
        return;
    }
    wire::StateV2 next = state_.current();
    next.globalStopped = true;
    if (estop) {
        next.estop = true;
    }
    if (emergency) {
        next.emergency = true;
    }
    updateState(next);
    ack(f.seq, ErrCode::Ok);
    pushStateIfChanged();
}

void GatewayCore::executeMoveAll(const wire::WireFrame& f)
{
    const auto result = rov_.move();
    if (!result) {
        ack(f.seq, mapRovFailure(result.failure));
        return;
    }
    wire::StateV2 next = state_.current();
    next.globalStopped = false;
    next.estop = false;     // D-02: MoveAll clears the emergency latches
    next.emergency = false;
    updateState(next);
    ack(f.seq, ErrCode::Ok);
    pushStateIfChanged();
}

void GatewayCore::executeGroupStopMove(const wire::WireFrame& f, bool vertical,
                                       bool stop)
{
    const auto result = vertical ? (stop ? rov_.stopVertical() : rov_.moveVertical())
                                 : (stop ? rov_.stopHorizontal()
                                         : rov_.moveHorizontal());
    if (!result) {
        ack(f.seq, mapRovFailure(result.failure));
        return;
    }
    wire::StateV2 next = state_.current();
    if (vertical) {
        next.verticalStopped = stop;
    } else {
        next.horizontalStopped = stop;
    }
    updateState(next);
    ack(f.seq, ErrCode::Ok);
    pushStateIfChanged();
}

void GatewayCore::executeSafe(const wire::WireFrame& f, bool on)
{
    if (on) {
        // Atomic linkage: stabilization first, Safe only after it holds
        // (Windows sends a single 0x0020 and expects the A35 to guarantee
        // the combined outcome).
        if (!state_.current().attitudeStab) {
            const auto enabled = rov_.enableStabilization();
            if (!enabled) {
                ack(f.seq, mapRovFailure(enabled.failure));
                return;
            }
            wire::StateV2 next = state_.current();
            next.attitudeStab = true;
            updateState(next);
        }
        wire::StateV2 next = state_.current();
        next.safe = true;
        updateState(next);
    } else {
        wire::StateV2 next = state_.current();
        next.safe = false; // SafeOff never disables stabilization
        updateState(next);
    }
    ack(f.seq, ErrCode::Ok);
    pushStateIfChanged();
}

void GatewayCore::executeStabilization(const wire::WireFrame& f, bool on)
{
    if (!on && state_.current().safe) {
        // Safe linkage: closing stabilization under Safe is rejected.
        ack(f.seq, ErrCode::Safety);
        return;
    }
    const auto result =
            on ? rov_.enableStabilization() : rov_.disableStabilization();
    if (!result) {
        ack(f.seq, mapRovFailure(result.failure));
        return;
    }
    wire::StateV2 next = state_.current();
    next.attitudeStab = on;
    updateState(next);
    ack(f.seq, ErrCode::Ok);
    pushStateIfChanged();
}

void GatewayCore::executeVerticalSync(const wire::WireFrame& f, bool on)
{
    // Pure A35-local state (U-01): vertical group semantics are assembled
    // from individual M33 commands; no M33 mode exists to toggle.
    wire::StateV2 next = state_.current();
    next.verticalSync = on;
    updateState(next);
    ack(f.seq, ErrCode::Ok);
    pushStateIfChanged();
}

void GatewayCore::executeHorizontalSync(const wire::WireFrame& f, bool on)
{
    const auto result = on ? rov_.enableHorizontalSynchronization()
                           : rov_.disableHorizontalSynchronization();
    if (!result) {
        // Toggle failed: keep the A35 mirror unchanged.
        ack(f.seq, mapRovFailure(result.failure));
        return;
    }
    wire::StateV2 next = state_.current();
    next.horizontalSync = on;
    updateState(next);
    ack(f.seq, ErrCode::Ok);
    pushStateIfChanged();
}

void GatewayCore::executeServoSet(const wire::WireFrame& f)
{
    wire::ServoSetCmd cmd;
    if (!wire::parseServoSet(f.payload, cmd)) {
        ack(f.seq, ErrCode::BadArg);
        return;
    }
    const auto result =
            rov_.setServo(cmd.id, static_cast<std::uint8_t>(cmd.angleDeg));
    ack(f.seq, result ? ErrCode::Ok : mapRovFailure(result.failure));
}

void GatewayCore::executeServoSetAll(const wire::WireFrame& f)
{
    std::uint16_t angle = 0U;
    if (!wire::parseServoSetAll(f.payload, angle)) {
        ack(f.seq, ErrCode::BadArg);
        return;
    }
    const auto result = rov_.setAllServos(static_cast<std::uint8_t>(angle));
    ack(f.seq, result ? ErrCode::Ok : mapRovFailure(result.failure));
}

void GatewayCore::executeServoMid(const wire::WireFrame& f)
{
    std::uint8_t id = 0U;
    if (!wire::parseServoMid(f.payload, id)) {
        ack(f.seq, ErrCode::BadArg);
        return;
    }
    const auto result =
            (id == wire::kIdBroadcast) ? rov_.centerAllServos()
                                       : rov_.centerServo(id);
    ack(f.seq, result ? ErrCode::Ok : mapRovFailure(result.failure));
}

void GatewayCore::executeServoGet(const wire::WireFrame& f, bool all)
{
    if (all) {
        const auto result = rov_.getAllServos();
        if (!result) {
            ack(f.seq, mapRovFailure(result.failure));
            return;
        }
        std::vector<std::int16_t> angles(result.value->begin(),
                                         result.value->end());
        ack(f.seq, ErrCode::Ok);
        const auto payload = wire::buildAngleList(angles);
        if (!payload.empty()) {
            OutboundFrame data;
            data.funcId = f.funcId;
            data.seq = f.seq; // echo the request seq (D-03)
            data.flags = wire::kFlagEvent;
            data.payload = payload;
            pushTelemetry(data);
        }
        return;
    }
    std::uint8_t id = 0U;
    if (!wire::parseServoGet(f.payload, id)) {
        ack(f.seq, ErrCode::BadArg);
        return;
    }
    const auto result = rov_.getServo(id);
    if (!result) {
        ack(f.seq, mapRovFailure(result.failure));
        return;
    }
    ack(f.seq, ErrCode::Ok);
    const auto payload = wire::buildAngleList({static_cast<std::int16_t>(
            *result.value)});
    if (!payload.empty()) {
        OutboundFrame data;
        data.funcId = f.funcId;
        data.seq = f.seq;
        data.flags = wire::kFlagEvent;
        data.payload = payload;
        pushTelemetry(data);
    }
}

void GatewayCore::executePropellerSet(const wire::WireFrame& f)
{
    wire::PropellerSetCmd cmd;
    if (!wire::parsePropellerSet(f.payload, cmd)) {
        ack(f.seq, ErrCode::BadArg);
        return;
    }
    const auto result = (cmd.id <= wire::kVerticalIdLast)
            ? sendVertical(cmd.id, clampSafe(cmd.valuePct))
            : sendHorizontal(cmd.id, clampSafe(cmd.valuePct));
    ack(f.seq, result ? ErrCode::Ok : mapRovFailure(result.failure));
}

void GatewayCore::executePropellerSetAll(const wire::WireFrame& f)
{
    std::int16_t value = 0;
    if (!wire::parsePropellerSetAll(f.payload, value)) {
        ack(f.seq, ErrCode::BadArg);
        return;
    }
    const std::int16_t clamped = clampSafe(value);
    // Vertical group first, then horizontal; on the first failure stop
    // executing and answer that error (partial group change, no success
    // ACK - protocol section 6).
    const auto rv = sendVerticalGroup(clamped);
    if (!rv) {
        ack(f.seq, mapRovFailure(rv.failure));
        return;
    }
    const auto rh = sendHorizontalGroup(clamped);
    if (!rh) {
        ack(f.seq, mapRovFailure(rh.failure));
        return;
    }
    ack(f.seq, ErrCode::Ok);
}

void GatewayCore::executePropellerStop(const wire::WireFrame& f)
{
    std::uint8_t id = 0U;
    if (!wire::parsePropellerStop(f.payload, id)) {
        ack(f.seq, ErrCode::BadArg);
        return;
    }
    // D-05: "base zero", not a stop latch. Broadcast zeroes both groups
    // through the current mode; a single channel is zeroed in whatever
    // form the current mode accepts.
    if (id == wire::kIdBroadcast) {
        const auto rv = sendVerticalGroup(0);
        if (!rv) {
            ack(f.seq, mapRovFailure(rv.failure));
            return;
        }
        const auto rh = sendHorizontalGroup(0);
        if (!rh) {
            ack(f.seq, mapRovFailure(rh.failure));
            return;
        }
        ack(f.seq, ErrCode::Ok);
        return;
    }
    const auto result = (id <= wire::kVerticalIdLast)
            ? sendVertical(id, 0)
            : sendHorizontal(id, 0);
    ack(f.seq, result ? ErrCode::Ok : mapRovFailure(result.failure));
}

void GatewayCore::executePropellerGet(const wire::WireFrame& f, bool base)
{
    std::uint8_t id = 0U;
    if (!wire::parsePropellerGet(f.payload, id)) {
        ack(f.seq, ErrCode::BadArg);
        return;
    }
    const auto result = base ? rov_.getPropellerBase(id)
                             : rov_.getPropellerOutput(id);
    if (!result) {
        // Mode-gated queries return M33 err safety (e.g. CH10-13 base with
        // stabilization off) - surfaced as-is.
        ack(f.seq, mapRovFailure(result.failure));
        return;
    }
    ack(f.seq, ErrCode::Ok);
    const auto payload = wire::buildPropellerList({*result.value});
    if (!payload.empty()) {
        OutboundFrame data;
        data.funcId = f.funcId;
        data.seq = f.seq;
        data.flags = wire::kFlagEvent;
        data.payload = payload;
        pushTelemetry(data);
    }
}

void GatewayCore::executeBaseValueVh(const wire::WireFrame& f)
{
    wire::BaseValueVhCmd cmd;
    if (!wire::parseBaseValueVh(f.payload, cmd)) {
        ack(f.seq, ErrCode::BadArg);
        return;
    }
    const std::int16_t vertical = clampSafe(cmd.verticalPct);
    const std::int16_t horizontal = clampSafe(cmd.horizontalPct);

    const auto rv = rov_.setVerticalBase(vertical);
    if (!rv) {
        ack(f.seq, mapRovFailure(rv.failure));
        return;
    }
    const auto rh = rov_.setHorizontalBase(horizontal);
    if (!rh) {
        // Partial success: global stop immediately, error ACK, high-level
        // alarm (VibePrompt section 6.4). Success is never reported when
        // only one group took the value.
        rov_.stop();
        wire::StateV2 next = state_.current();
        next.globalStopped = true;
        updateState(next);
        const auto code = mapRovFailure(rh.failure);
        ack(f.seq, code);
        pushAlarm(2U, static_cast<std::uint16_t>(code),
                  "base value vh partial failure: global stop executed");
        pushStateIfChanged();
        return;
    }
    ack(f.seq, ErrCode::Ok);
}

void GatewayCore::executeSensorQuery(const wire::WireFrame& f)
{
    switch (static_cast<wire::Func>(f.funcId)) {
    case wire::Func::SensorMpu: {
        const auto result = rov_.readMpu();
        if (!result) {
            ack(f.seq, mapRovFailure(result.failure));
            return;
        }
        break;
    }
    case wire::Func::SensorDyp: {
        const auto result = rov_.readDyp();
        if (!result) {
            ack(f.seq, mapRovFailure(result.failure));
            return;
        }
        break;
    }
    case wire::Func::SensorAll: {
        const auto result = rov_.getSensorSnapshot();
        if (!result) {
            ack(f.seq, mapRovFailure(result.failure));
            return;
        }
        break;
    }
    default:
        ack(f.seq, ErrCode::BadCmd);
        return;
    }
    ack(f.seq, ErrCode::Ok);
    // Query side effects: refresh the relevant cache and push the latest
    // SensorSummary (D-14: no per-query data payload exists on the wire).
    if (sensorBridge_ != nullptr) {
        switch (static_cast<wire::Func>(f.funcId)) {
        case wire::Func::SensorMpu:
            sensorBridge_->refreshMpu();
            sensorBridge_->pushLatestSummary();
            break;
        case wire::Func::SensorDyp:
            sensorBridge_->refreshDyp();
            sensorBridge_->pushLatestSummary();
            break;
        case wire::Func::SensorAll:
            sensorBridge_->refreshSnapshot();
            sensorBridge_->pushLatestSummary();
            break;
        default:
            break;
        }
    }
}

// ---- thruster translation ---------------------------------------------------

rov::RovResult<void> GatewayCore::sendVertical(std::uint8_t id,
                                               std::int16_t value)
{
    if (state_.current().attitudeStab) {
        return rov_.setVerticalBase(value);
    }
    return rov_.setVerticalPropeller(id, value);
}

rov::RovResult<void> GatewayCore::sendHorizontal(std::uint8_t id,
                                                 std::int16_t value)
{
    if (state_.current().horizontalSync) {
        return rov_.setHorizontalBase(value);
    }
    return rov_.setHorizontalPropeller(id, value);
}

rov::RovResult<void> GatewayCore::sendVerticalGroup(std::int16_t value)
{
    if (state_.current().attitudeStab) {
        return rov_.setVerticalBase(value); // one call drives CH10-13
    }
    for (std::uint8_t id = wire::kVerticalIdFirst;
         id <= wire::kVerticalIdLast; ++id) {
        const auto result = rov_.setVerticalPropeller(id, value);
        if (!result) {
            return result;
        }
    }
    return rov::RovResult<void>::success();
}

rov::RovResult<void> GatewayCore::sendHorizontalGroup(std::int16_t value)
{
    if (state_.current().horizontalSync) {
        return rov_.setHorizontalBase(value); // one call drives CH14-15
    }
    for (std::uint8_t id = wire::kHorizontalIdFirst;
         id <= wire::kHorizontalIdLast; ++id) {
        const auto result = rov_.setHorizontalPropeller(id, value);
        if (!result) {
            return result;
        }
    }
    return rov::RovResult<void>::success();
}

std::int16_t GatewayCore::clampSafe(std::int16_t valuePct) const
{
    // The clamp is Safe-mode policy (protocol 6.1): outside Safe the value
    // passes through unchanged.
    if (!state_.current().safe) {
        return valuePct;
    }
    const int limit = config_.safeLimitPct;
    if (limit >= 100) {
        return valuePct;
    }
    if (valuePct > static_cast<std::int16_t>(limit)) {
        return static_cast<std::int16_t>(limit);
    }
    if (valuePct < static_cast<std::int16_t>(-limit)) {
        return static_cast<std::int16_t>(-limit);
    }
    return valuePct;
}

// ---- state / outbound helpers ------------------------------------------------

void GatewayCore::updateState(const wire::StateV2& next)
{
    if (!state_.apply(next)) {
        // Should be unreachable: transitions above never build the illegal
        // combination. Keep the previous state and alarm loudly.
        pushAlarm(2U, static_cast<std::uint16_t>(ErrCode::Safety),
                  "illegal state transition rejected (safe without stabilization)");
    }
}

void GatewayCore::ack(std::uint16_t seq, ErrCode code)
{
    OutboundFrame frame;
    frame.funcId = static_cast<std::uint16_t>(wire::Func::Ack);
    frame.seq = seq; // Windows seq echoed verbatim
    frame.flags = 0U; // D-03: ACK flags zero, matching the mock A35
    frame.payload = wire::buildAck(code);
    pushTelemetry(frame);
}

void GatewayCore::pushStateIfChanged(bool forced)
{
    if (forced) {
        state_.forcePush();
    }
    wire::StateV2 snapshot;
    if (!state_.consumeChanged(snapshot)) {
        return;
    }
    OutboundFrame frame;
    frame.funcId = static_cast<std::uint16_t>(wire::Func::StateEventV2);
    frame.seq = 0U;
    frame.flags = wire::kFlagEvent;
    frame.payload = wire::buildStateEventV2(snapshot);
    pushTelemetry(frame);
}

void GatewayCore::pushAlarm(std::uint8_t level, std::uint16_t code,
                            const char* text)
{
    OutboundFrame frame;
    frame.funcId = static_cast<std::uint16_t>(wire::Func::AlarmEvent);
    frame.seq = 0U;
    frame.flags = wire::kFlagEvent;
    const auto nowMs = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count()
            & 0xFFFFFFFFU);
    frame.payload = wire::buildAlarmEvent(level, code, nowMs, text);
    pushTelemetry(frame);
}

std::vector<OutboundFrame> GatewayCore::takeOutboundFrames()
{
    std::lock_guard<std::mutex> lock(outboundMutex_);
    std::vector<OutboundFrame> frames(outbound_.begin(), outbound_.end());
    outbound_.clear();
    return frames;
}

void GatewayCore::pushTelemetry(const OutboundFrame& frame)
{
    std::lock_guard<std::mutex> lock(outboundMutex_);
    outbound_.push_back(frame);
    // Bounded queue: with a disconnected client the 100 Hz summary stream
    // would otherwise grow without limit. Newest-wins (oldest dropped).
    while (outbound_.size() > config_.outboundQueueCapacity) {
        outbound_.pop_front();
    }
}

// ---- error mapping (DECISIONS D-06) -------------------------------------------

wire::ErrCode GatewayCore::mapRovFailure(const rov::RovFailure& failure)
{
    switch (failure.code) {
    case rov::RovError::None:
        return ErrCode::Ok;
    case rov::RovError::BadArgument:
        return ErrCode::BadArg;
    case rov::RovError::BadCommand:
        return ErrCode::BadCmd;
    case rov::RovError::Busy:
        return ErrCode::Busy;
    case rov::RovError::NotReady:
    case rov::RovError::Disconnected:
        return ErrCode::NotReady;
    case rov::RovError::Timeout:
    case rov::RovError::Io:
    case rov::RovError::ProtocolError:
    case rov::RovError::TransportIo:
        return ErrCode::Timeout; // real cause lands in the log/alarm
    case rov::RovError::Unsupported:
        return ErrCode::Unsupported;
    case rov::RovError::Safety:
        return ErrCode::Safety;
    case rov::RovError::SequenceExhausted:
        return ErrCode::Busy;
    }
    return ErrCode::BadCmd;
}

} // namespace gw
