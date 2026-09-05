// GatewayCore dispatch: ACK semantics, state events, Safe linkage, stop/move
// latches, emergency paths, thruster translation, base-value atomicity,
// startup alignment, disconnect cleanup, heartbeat liveness.
#include "fake_rov_control.hpp"
#include "test_support.hpp"

#include "core/gateway_core.hpp"
#include "wire/function_registry.hpp"

#include <cstring>
#include <memory>

namespace {

std::vector<std::uint8_t> payloadOf(std::initializer_list<std::uint8_t> b)
{
    return std::vector<std::uint8_t>(b);
}

gw::wire::WireFrame request(std::uint16_t funcId, std::uint16_t seq,
                            std::vector<std::uint8_t> payload = {})
{
    gw::wire::WireFrame f;
    f.funcId = funcId;
    f.seq = seq;
    f.flags = gw::wire::kFlagNeedAck;
    f.payload = std::move(payload);
    return f;
}

int ackCodeFor(const std::vector<gw::OutboundFrame>& frames, std::uint16_t seq)
{
    for (const auto& f : frames) {
        if ((f.funcId == static_cast<std::uint16_t>(gw::wire::Func::Ack))
            && (f.seq == seq)) {
            std::uint16_t code = 0U;
            if (gw::wire::getU16(f.payload, 0U, code)) {
                return static_cast<int>(code);
            }
        }
    }
    return -1;
}

int countFrames(const std::vector<gw::OutboundFrame>& frames,
                std::uint16_t funcId)
{
    int n = 0;
    for (const auto& f : frames) {
        if (f.funcId == funcId) {
            ++n;
        }
    }
    return n;
}

const gw::OutboundFrame* findFrame(const std::vector<gw::OutboundFrame>& frames,
                                   std::uint16_t funcId)
{
    for (const auto& f : frames) {
        if (f.funcId == funcId) {
            return &f;
        }
    }
    return nullptr;
}

// StateEventV2 mask of the last event in the batch.
std::uint16_t lastStateMask(const std::vector<gw::OutboundFrame>& frames)
{
    const gw::OutboundFrame* last = nullptr;
    for (const auto& f : frames) {
        if (f.funcId == static_cast<std::uint16_t>(gw::wire::Func::StateEventV2)) {
            last = &f;
        }
    }
    if (last == nullptr) {
        return 0xFFFFU; // sentinel: no event
    }
    std::uint16_t mask = 0U;
    gw::wire::getU16(last->payload, 1U, mask);
    return mask;
}

std::uint16_t F(gw::wire::Func f)
{
    return static_cast<std::uint16_t>(f);
}

struct Harness
{
    gw::FakeRovControl rov;
    gw::GatewayConfig cfg;
    std::unique_ptr<gw::GatewayCore> core;

    explicit Harness(gw::GatewayConfig config = gw::GatewayConfig())
        : cfg(config)
    {
        core = std::make_unique<gw::GatewayCore>(rov, cfg);
        core->alignStartupState();
        core->takeOutboundFrames(); // drop alignment-time state pushes
        rov.calls.clear(); // tests count post-alignment traffic only
    }

    std::vector<gw::OutboundFrame> send(std::uint16_t funcId, std::uint16_t seq,
                                        std::vector<std::uint8_t> payload = {})
    {
        core->submitRequest(request(funcId, seq, std::move(payload)));
        core->drainQueue();
        return core->takeOutboundFrames();
    }
};

} // namespace

int main()
{
    using namespace gw;
    using wire::ErrCode;
    using SB = wire::StateV2;

    // ---- startup alignment (U-02/U-03) ------------------------------------
    {
        FakeRovControl rov;
        rov.globalStopped = true; // rov_self_test leaves the latch set
        GatewayCore core(rov);
        CHECK(core.alignStartupState());
        CHECK(core.state().attitudeStab);   // stabilization forced on
        CHECK(core.state().horizontalSync); // M33 synchronization on
        CHECK(core.state().verticalSync);   // A35-local vertical sync on
        CHECK(core.state().globalStopped);  // mirrored from telemetry
        CHECK(!core.state().safe);
        CHECK(!core.state().estop);
        CHECK(!core.state().emergency);
        CHECK(rov.called("getStabilization"));
        CHECK(rov.called("enableStabilization"));
        CHECK(rov.called("enableHorizontalSynchronization"));
        // Alignment consumes the M33 latch state, not assumptions.
        FakeRovControl rov2;
        rov2.stabEnabled = false;
        rov2.verticalStopped = true;
        GatewayCore core2(rov2);
        CHECK(core2.alignStartupState());
        CHECK(core2.state().attitudeStab); // forced on regardless
        CHECK(core2.state().verticalStopped);
    }
    { // alignment failure aborts (caller retries / refuses to serve)
        FakeRovControl rov;
        rov.failNext("enableStabilization", rov::RovError::Safety);
        GatewayCore core(rov);
        CHECK(!core.alignStartupState());
    }

    // ---- client lifecycle ---------------------------------------------------
    {
        Harness h;
        h.core->onClientConnected();
        auto frames = h.core->takeOutboundFrames();
        CHECK_EQ(countFrames(frames, F(wire::Func::StateEventV2)), 1);
        const std::uint16_t mask = lastStateMask(frames);
        CHECK_EQ(mask & wire::kStateV2AttitudeStab, wire::kStateV2AttitudeStab);
        CHECK_EQ(mask & wire::kStateV2VerticalSync, wire::kStateV2VerticalSync);
        CHECK_EQ(mask & wire::kStateV2HorizontalSync, wire::kStateV2HorizontalSync);
        CHECK_EQ(static_cast<std::uint16_t>(mask & wire::kStateV2Safe),
                 static_cast<std::uint16_t>(0U));

        // Disconnect: pending commands dropped (no replay), emergency stop.
        h.core->submitRequest(request(F(wire::Func::ServoSet), 7U,
                                      payloadOf({0x01, 0x5A, 0x00})));
        CHECK(h.rov.calls.empty()); // queued, not executed yet
        h.core->onClientDisconnected();
        h.core->drainQueue();
        CHECK(!h.rov.called("setServo")); // queued command was dropped
        CHECK(h.rov.called("stop")); // stop_on_disconnect default
        CHECK(h.core->state().globalStopped);
        CHECK(h.core->takeOutboundFrames().empty()); // nothing to send
    }
    { // stopOnDisconnect disabled by config
        GatewayConfig cfg;
        cfg.stopOnDisconnect = false;
        Harness h(cfg);
        h.core->onClientConnected();
        h.core->onClientDisconnected();
        CHECK(!h.rov.called("stop"));
    }

    // ---- every request function gets exactly one ACK with seq echo -------
    {
        for (const wire::FunctionEntry& entry : wire::registry()) {
            if (entry.direction != wire::Direction::Request) {
                continue;
            }
            if (entry.funcId == wire::Func::Heartbeat) {
                continue; // no ACK by protocol
            }
            Harness h;
            h.core->onClientConnected();
            h.core->takeOutboundFrames();
            // Build a minimally valid payload per function.
            std::vector<std::uint8_t> payload;
            switch (entry.funcId) {
            case wire::Func::ServoSet: payload = payloadOf({0x00, 0x5A, 0x00}); break;
            case wire::Func::ServoSetAll: payload = payloadOf({0x5A, 0x00}); break;
            case wire::Func::ServoMid: payload = payloadOf({0x00}); break;
            case wire::Func::ServoGet: payload = payloadOf({0x00}); break;
            case wire::Func::PropellerSet: payload = payloadOf({0x0A, 0x0A, 0x00}); break;
            case wire::Func::PropellerSetAll: payload = payloadOf({0x0A, 0x00}); break;
            case wire::Func::PropellerStop: payload = payloadOf({0xFF}); break;
            case wire::Func::PropellerGetBase:
            case wire::Func::PropellerGetReal: payload = payloadOf({0x0A}); break;
            case wire::Func::BaseValueVH: payload = payloadOf({0x0A, 0x00, 0x0A, 0x00}); break;
            default: break; // empty-payload functions
            }
            const std::uint16_t seq = 0x1234U;
            const auto frames = h.send(F(entry.funcId), seq, payload);
            CHECK_EQ(countFrames(frames, F(wire::Func::Ack)), 1);
            if (entry.funcId == wire::Func::BaseValue) {
                CHECK_EQ(ackCodeFor(frames, seq),
                         static_cast<int>(ErrCode::Unsupported));
            } else {
                CHECK_EQ(ackCodeFor(frames, seq),
                         static_cast<int>(ErrCode::Ok));
            }
        }
    }

    // ---- Stop/Estop/Emergency share execution, differ in bits ------------
    {
        Harness h;
        auto frames = h.send(F(wire::Func::Estop), 9U);
        CHECK_EQ(ackCodeFor(frames, 9U), static_cast<int>(ErrCode::Ok));
        CHECK(h.rov.called("stop"));
        CHECK_EQ(h.rov.countCalls("setServo"), 0U); // never touches servos
        CHECK_EQ(h.rov.countCalls("setAllServos"), 0U);
        SB s = h.core->state();
        CHECK(s.estop && s.globalStopped && !s.emergency);
        CHECK_EQ(lastStateMask(frames) & wire::kStateV2Estop, wire::kStateV2Estop);

        frames = h.send(F(wire::Func::Emergency), 10U);
        CHECK_EQ(ackCodeFor(frames, 10U), static_cast<int>(ErrCode::Ok));
        s = h.core->state();
        CHECK(s.emergency && s.estop && s.globalStopped);

        frames = h.send(F(wire::Func::StopAll), 11U);
        CHECK_EQ(ackCodeFor(frames, 11U), static_cast<int>(ErrCode::Ok));
        CHECK_EQ(h.rov.countCalls("stop"), 3U); // one per command
        s = h.core->state();
        CHECK(s.globalStopped && s.estop && s.emergency); // latches persist
    }
    { // Stop under stabilization stays a plain stop (U-05: no workaround)
        Harness h;
        auto frames = h.send(F(wire::Func::StopAll), 1U);
        CHECK_EQ(ackCodeFor(frames, 1U), static_cast<int>(ErrCode::Ok));
        CHECK(h.rov.called("stop"));
        CHECK(!h.rov.called("disableStabilization"));
        CHECK(h.core->state().attitudeStab); // mode untouched by stop
    }

    // ---- MoveAll clears the emergency latches (D-02) ----------------------
    {
        Harness h;
        h.send(F(wire::Func::Estop), 1U);
        auto frames = h.send(F(wire::Func::MoveAll), 2U);
        CHECK_EQ(ackCodeFor(frames, 2U), static_cast<int>(ErrCode::Ok));
        CHECK(h.rov.called("move"));
        SB s = h.core->state();
        CHECK(!s.globalStopped && !s.estop && !s.emergency);
    }
    { // group move does NOT clear estop
        Harness h;
        h.send(F(wire::Func::Estop), 1U);
        h.send(F(wire::Func::MoveVertical), 2U);
        SB s = h.core->state();
        CHECK(s.estop && !s.verticalStopped);
    }

    // ---- group stop/move latches -------------------------------------------
    {
        Harness h;
        auto frames = h.send(F(wire::Func::StopVertical), 1U);
        CHECK_EQ(ackCodeFor(frames, 1U), static_cast<int>(ErrCode::Ok));
        CHECK(h.rov.called("stopVertical"));
        CHECK(h.core->state().verticalStopped);
        h.send(F(wire::Func::MoveVertical), 2U);
        CHECK(!h.core->state().verticalStopped);
        h.send(F(wire::Func::StopHorizontal), 3U);
        CHECK(h.core->state().horizontalStopped);
        CHECK(h.rov.called("stopHorizontal"));
        h.send(F(wire::Func::MoveHorizontal), 4U);
        CHECK(!h.core->state().horizontalStopped);
    }

    // ---- Safe linkage -------------------------------------------------------
    {
        // SafeOn with stabilization already on: no extra M33 call.
        Harness h;
        auto frames = h.send(F(wire::Func::SafeOn), 1U);
        CHECK_EQ(ackCodeFor(frames, 1U), static_cast<int>(ErrCode::Ok));
        CHECK_EQ(h.rov.countCalls("enableStabilization"), 0U); // aligned state already on
        SB s = h.core->state();
        CHECK(s.safe && s.attitudeStab);
        CHECK_EQ(lastStateMask(frames) & wire::kStateV2Safe, wire::kStateV2Safe);

        // Safe ON: closing stabilization is rejected.
        frames = h.send(F(wire::Func::HorizontalOff), 2U);
        CHECK_EQ(ackCodeFor(frames, 2U), static_cast<int>(ErrCode::Safety));
        CHECK_EQ(h.rov.countCalls("disableStabilization"), 0U);
        CHECK(h.core->state().attitudeStab);

        // Safe clamp: propeller value limited while Safe is on.
        frames = h.send(F(wire::Func::PropellerSet), 3U,
                        payloadOf({0x0A, 0x64, 0x00})); // CH10, +100
        CHECK_EQ(ackCodeFor(frames, 3U), static_cast<int>(ErrCode::Ok));
        CHECK_EQ(h.rov.countCalls("setVerticalBase"), 1U);
        CHECK_EQ(h.rov.verticalBase, 30); // clamped to safeLimitPct

        // SafeOff releases the clamp and never disables stabilization.
        h.send(F(wire::Func::SafeOff), 4U);
        CHECK(!h.core->state().safe);
        CHECK(h.core->state().attitudeStab);
        h.send(F(wire::Func::PropellerSet), 5U, payloadOf({0x0A, 0x64, 0x00}));
        CHECK_EQ(h.rov.verticalBase, 100); // unclamped
    }
    {
        // SafeOn with stabilization off: stabilization is enabled first.
        Harness h;
        h.send(F(wire::Func::HorizontalOff), 1U); // safe off -> allowed
        CHECK(!h.core->state().attitudeStab);
        auto frames = h.send(F(wire::Func::SafeOn), 2U);
        CHECK_EQ(ackCodeFor(frames, 2U), static_cast<int>(ErrCode::Ok));
        CHECK_EQ(h.rov.countCalls("enableStabilization"), 1U);
        SB s = h.core->state();
        CHECK(s.safe && s.attitudeStab);
    }
    {
        // SafeOn when stabilization enable fails: error ACK, safe stays off.
        Harness h;
        h.send(F(wire::Func::HorizontalOff), 1U);
        h.rov.failNext("enableStabilization", rov::RovError::Safety);
        auto frames = h.send(F(wire::Func::SafeOn), 2U);
        CHECK_EQ(ackCodeFor(frames, 2U), static_cast<int>(ErrCode::Safety));
        CHECK(!h.core->state().safe);
        CHECK(!h.core->state().attitudeStab);
    }

    // ---- thruster translation matrix (U-01) ---------------------------------
    {
        // stabilization ON: vertical group value -> setVerticalBase
        Harness h;
        h.send(F(wire::Func::PropellerSet), 1U, payloadOf({0x0B, 0x32, 0x00}));
        CHECK_EQ(h.rov.countCalls("setVerticalBase"), 1U);
        CHECK_EQ(h.rov.verticalBase, 50);
        CHECK_EQ(h.rov.countCalls("setVerticalPropeller"), 0U);

        // horizontal sync ON: horizontal value -> setHorizontalBase
        h.send(F(wire::Func::PropellerSet), 2U, payloadOf({0x0E, 0x32, 0x00}));
        CHECK_EQ(h.rov.countCalls("setHorizontalBase"), 1U);
        CHECK_EQ(h.rov.horizontalBase, 50);
        CHECK_EQ(h.rov.countCalls("setHorizontalPropeller"), 0U);
    }
    {
        // stabilization OFF: vertical individual passes through; the M33
        // would reject nothing because the group semantic was assembled.
        Harness h;
        h.send(F(wire::Func::HorizontalOff), 1U);
        auto frames = h.send(F(wire::Func::PropellerSet), 2U,
                             payloadOf({0x0A, 0x9C, 0xFF})); // CH10, -100
        CHECK_EQ(ackCodeFor(frames, 2U), static_cast<int>(ErrCode::Ok));
        CHECK_EQ(h.rov.countCalls("setVerticalPropeller"), 1U);
        CHECK_EQ(h.rov.vertical[0], -100);
        CHECK_EQ(h.rov.countCalls("setVerticalBase"), 0U);
        // horizontal still synced -> base
        h.send(F(wire::Func::PropellerSet), 3U, payloadOf({0x0F, 0x10, 0x00}));
        CHECK_EQ(h.rov.countCalls("setHorizontalBase"), 1U);
    }
    {
        // horizontal sync OFF: horizontal individual passes through.
        Harness h;
        h.send(F(wire::Func::HorizontalSyncOff), 1U);
        h.send(F(wire::Func::PropellerSet), 2U, payloadOf({0x0E, 0x14, 0x00}));
        CHECK_EQ(h.rov.countCalls("setHorizontalPropeller"), 1U);
        CHECK_EQ(h.rov.horizontal[0], 20);
    }
    {
        // set propeller all: base modes collapse to one call per group.
        Harness h;
        auto frames = h.send(F(wire::Func::PropellerSetAll), 1U,
                             payloadOf({0x14, 0x00})); // +20
        CHECK_EQ(ackCodeFor(frames, 1U), static_cast<int>(ErrCode::Ok));
        CHECK_EQ(h.rov.countCalls("setVerticalBase"), 1U);
        CHECK_EQ(h.rov.countCalls("setHorizontalBase"), 1U);
    }
    {
        // set propeller all with both modes off: 4 + 2 individual calls.
        Harness h;
        h.send(F(wire::Func::HorizontalOff), 1U);
        h.send(F(wire::Func::HorizontalSyncOff), 2U);
        h.send(F(wire::Func::PropellerSetAll), 3U, payloadOf({0x0A, 0x00}));
        CHECK_EQ(h.rov.countCalls("setVerticalPropeller"), 4U);
        CHECK_EQ(h.rov.countCalls("setHorizontalPropeller"), 2U);
    }
    {
        // 0x0042 propeller stop, broadcast, base modes: two zeroed bases.
        Harness h;
        h.send(F(wire::Func::PropellerSet), 1U, payloadOf({0x0A, 0x64, 0x00}));
        h.send(F(wire::Func::PropellerSet), 2U, payloadOf({0x0E, 0x64, 0x00}));
        auto frames = h.send(F(wire::Func::PropellerStop), 3U, payloadOf({0xFF}));
        CHECK_EQ(ackCodeFor(frames, 3U), static_cast<int>(ErrCode::Ok));
        CHECK_EQ(h.rov.verticalBase, 0);
        CHECK_EQ(h.rov.horizontalBase, 0);
        // single channel in individual mode
        h.send(F(wire::Func::HorizontalSyncOff), 4U);
        h.send(F(wire::Func::PropellerStop), 5U, payloadOf({0x0F}));
        CHECK_EQ(h.rov.horizontal[1], 0);
    }
    {
        // 0x0042 under a stop latch: M33 rejects set commands; forwarded.
        Harness h;
        h.send(F(wire::Func::StopAll), 1U);
        auto frames = h.send(F(wire::Func::PropellerStop), 2U, payloadOf({0xFF}));
        CHECK_EQ(ackCodeFor(frames, 2U), static_cast<int>(ErrCode::Safety));
    }

    // ---- base value vh atomicity --------------------------------------------
    {
        Harness h;
        auto frames = h.send(F(wire::Func::BaseValueVH), 1U,
                             payloadOf({0x0A, 0x00, 0xF4, 0xFF})); // 10, -12
        CHECK_EQ(ackCodeFor(frames, 1U), static_cast<int>(ErrCode::Ok));
        CHECK_EQ(h.rov.verticalBase, 10);
        CHECK_EQ(h.rov.horizontalBase, -12);
        CHECK_EQ(countFrames(frames, F(wire::Func::AlarmEvent)), 0);
        CHECK(!h.rov.called("stop"));
    }
    {
        // Second call fails: global stop + error ACK + high-level alarm.
        Harness h;
        h.rov.failNext("setHorizontalBase", rov::RovError::Safety);
        auto frames = h.send(F(wire::Func::BaseValueVH), 1U,
                             payloadOf({0x0A, 0x00, 0x0A, 0x00}));
        CHECK_EQ(ackCodeFor(frames, 1U), static_cast<int>(ErrCode::Safety));
        CHECK(h.rov.called("stop"));
        CHECK(h.core->state().globalStopped);
        CHECK_EQ(countFrames(frames, F(wire::Func::AlarmEvent)), 1);
        const auto* alarm = findFrame(frames, F(wire::Func::AlarmEvent));
        CHECK_EQ(alarm->payload[0], 2U); // level Error
    }
    {
        // First call fails: error ACK only, nothing to roll back.
        Harness h;
        h.rov.failNext("setVerticalBase", rov::RovError::Busy);
        auto frames = h.send(F(wire::Func::BaseValueVH), 1U,
                             payloadOf({0x0A, 0x00, 0x0A, 0x00}));
        CHECK_EQ(ackCodeFor(frames, 1U), static_cast<int>(ErrCode::Busy));
        CHECK(!h.rov.called("stop"));
        CHECK_EQ(countFrames(frames, F(wire::Func::AlarmEvent)), 0);
    }

    // ---- servos: never gated by safe/stop/emergency -------------------------
    {
        Harness h;
        h.send(F(wire::Func::Estop), 1U);
        h.send(F(wire::Func::SafeOn), 2U);
        auto frames = h.send(F(wire::Func::ServoSet), 3U,
                             payloadOf({0x05, 0xB4, 0x00})); // id 5, 180
        CHECK_EQ(ackCodeFor(frames, 3U), static_cast<int>(ErrCode::Ok));
        CHECK_EQ(h.rov.servoAngles[5], 180U);

        frames = h.send(F(wire::Func::ServoGet), 4U, payloadOf({0x05}));
        CHECK_EQ(ackCodeFor(frames, 4U), static_cast<int>(ErrCode::Ok));
        const auto* data = findFrame(frames, F(wire::Func::ServoGet));
        CHECK(data != nullptr);
        CHECK_EQ(data->seq, 4U); // data frame echoes the request seq
        std::int16_t angle = 0;
        CHECK(wire::getI16(data->payload, 0U, angle));
        CHECK_EQ(angle, 180);

        frames = h.send(F(wire::Func::ServoGetAll), 5U);
        data = findFrame(frames, F(wire::Func::ServoGetAll));
        CHECK(data != nullptr);
        CHECK_EQ(data->payload.size(), static_cast<std::size_t>(20U));

        h.send(F(wire::Func::ServoMid), 6U, payloadOf({0x05}));
        CHECK_EQ(h.rov.servoAngles[5], 90U);
        h.send(F(wire::Func::ServoMid), 7U, payloadOf({0xFF}));
        CHECK(h.rov.called("centerAllServos"));
        h.send(F(wire::Func::ServoSetAll), 8U, payloadOf({0x2D, 0x00})); // 45
        CHECK(h.rov.called("setAllServos"));
    }

    // ---- propeller queries ----------------------------------------------------
    {
        Harness h;
        h.send(F(wire::Func::PropellerSet), 1U, payloadOf({0x0A, 0x1E, 0x00}));
        auto frames = h.send(F(wire::Func::PropellerGetBase), 2U,
                             payloadOf({0x0A}));
        CHECK_EQ(ackCodeFor(frames, 2U), static_cast<int>(ErrCode::Ok));
        const auto* data = findFrame(frames, F(wire::Func::PropellerGetBase));
        CHECK(data != nullptr);
        std::int16_t v = 0;
        CHECK(wire::getI16(data->payload, 0U, v));
        CHECK_EQ(v, 30);

        // base query for horizontal channel with sync off -> safety
        h.send(F(wire::Func::HorizontalSyncOff), 3U);
        frames = h.send(F(wire::Func::PropellerGetBase), 4U,
                        payloadOf({0x0E}));
        CHECK_EQ(ackCodeFor(frames, 4U), static_cast<int>(ErrCode::Safety));
        CHECK(findFrame(frames, F(wire::Func::PropellerGetBase)) == nullptr);
    }

    // ---- sensors -----------------------------------------------------------
    {
        Harness h;
        auto frames = h.send(F(wire::Func::SensorMpu), 1U);
        CHECK_EQ(ackCodeFor(frames, 1U), static_cast<int>(ErrCode::Ok));
        CHECK(h.rov.called("readMpu"));

        h.rov.dypBusy = true;
        frames = h.send(F(wire::Func::SensorDyp), 2U);
        CHECK_EQ(ackCodeFor(frames, 2U), static_cast<int>(ErrCode::Busy));
        h.rov.dypBusy = false;
        frames = h.send(F(wire::Func::SensorDyp), 3U);
        CHECK_EQ(ackCodeFor(frames, 3U), static_cast<int>(ErrCode::Ok));

        frames = h.send(F(wire::Func::SensorAll), 4U);
        CHECK_EQ(ackCodeFor(frames, 4U), static_cast<int>(ErrCode::Ok));
        CHECK(h.rov.called("getSensorSnapshot"));
    }

    // ---- system queries ------------------------------------------------------
    {
        Harness h;
        auto frames = h.send(F(wire::Func::Ask), 1U);
        CHECK_EQ(ackCodeFor(frames, 1U), static_cast<int>(ErrCode::Ok));
        CHECK_EQ(frames.size(), static_cast<std::size_t>(1U)); // ack only

        frames = h.send(F(wire::Func::Ver), 2U);
        CHECK_EQ(frames.size(), static_cast<std::size_t>(1U));

        frames = h.send(F(wire::Func::Help), 3U);
        CHECK_EQ(frames.size(), static_cast<std::size_t>(1U));

        frames = h.send(F(wire::Func::Status), 4U);
        CHECK_EQ(ackCodeFor(frames, 4U), static_cast<int>(ErrCode::Ok));
        CHECK_EQ(countFrames(frames, F(wire::Func::StateEventV2)), 1);
    }

    // ---- unknown / malformed / wrong-direction robustness --------------------
    {
        Harness h;
        auto frames = h.send(0x0099U, 1U);
        CHECK_EQ(ackCodeFor(frames, 1U), static_cast<int>(ErrCode::Unsupported));

        // unknown without NeedAck: silent, no crash
        h.core->submitRequest([] {
            wire::WireFrame f;
            f.funcId = 0x0099U;
            f.seq = 2U;
            f.flags = 0U;
            return f;
        }());
        h.core->drainQueue();
        CHECK(h.core->takeOutboundFrames().empty());

        // malformed payloads -> bad_arg
        frames = h.send(F(wire::Func::ServoSet), 3U, payloadOf({0x05, 0x5A}));
        CHECK_EQ(ackCodeFor(frames, 3U), static_cast<int>(ErrCode::BadArg));
        frames = h.send(F(wire::Func::PropellerSet), 4U,
                        payloadOf({0x0A, 0x65, 0x00})); // +101
        CHECK_EQ(ackCodeFor(frames, 4U), static_cast<int>(ErrCode::BadArg));
        frames = h.send(F(wire::Func::PropellerSet), 5U,
                        payloadOf({0x09, 0x00, 0x00})); // id 9
        CHECK_EQ(ackCodeFor(frames, 5U), static_cast<int>(ErrCode::BadArg));

        // our own output frames arriving inbound are ignored
        h.core->submitRequest([] {
            wire::WireFrame f;
            f.funcId = F(wire::Func::StateEventV2);
            f.flags = wire::kFlagEvent;
            return f;
        }());
        h.core->drainQueue();
        CHECK(h.core->takeOutboundFrames().empty());
    }

    // ---- priority: estop executes ahead of queued normal commands -----------
    {
        Harness h;
        for (std::uint16_t i = 1; i <= 30U; ++i) {
            h.core->submitRequest(request(F(wire::Func::ServoSet), i,
                                          payloadOf({0x00, 0x5A, 0x00})));
        }
        h.core->submitRequest(request(F(wire::Func::Estop), 99U));
        h.core->drainQueue();
        CHECK_EQ(h.rov.calls.front(), "stop"); // estop jumped the queue
        CHECK_EQ(h.rov.countCalls("stop"), 1U);
        CHECK_EQ(h.rov.countCalls("setServo"), 30U);
    }

    // ---- heartbeat liveness ---------------------------------------------------
    {
        GatewayConfig cfg;
        cfg.heartbeatTimeoutMs = 5000;
        Harness h(cfg);
        h.core->onClientConnected();
        h.core->takeOutboundFrames(); // consume the connect-time state push

        wire::WireFrame hb;
        hb.funcId = F(wire::Func::Heartbeat);
        hb.flags = 0U;
        hb.payload = payloadOf({0x01, 0x02, 0x03, 0x04});
        h.core->submitRequest(hb);
        h.core->drainQueue();
        CHECK(h.core->takeOutboundFrames().empty()); // silent heartbeat
        CHECK(!h.core->heartbeatExpired());

        using namespace std::chrono;
        h.core->setLastHeartbeatForTest(
                steady_clock::now() - milliseconds(6000));
        CHECK(h.core->heartbeatExpired());

        // disabled by config
        GatewayConfig off;
        off.heartbeatTimeoutMs = 0;
        Harness h2(off);
        h2.core->onClientConnected();
        h2.core->setLastHeartbeatForTest(
                steady_clock::now() - milliseconds(60000));
        CHECK(!h2.core->heartbeatExpired());
    }

    // ---- periodic full-state push --------------------------------------------
    {
        Harness h;
        h.core->onClientConnected();
        h.core->takeOutboundFrames();
        h.core->tickPeriodic(); // unchanged state still re-pushed at 1Hz
        auto frames = h.core->takeOutboundFrames();
        CHECK_EQ(countFrames(frames, F(wire::Func::StateEventV2)), 1);
        h.core->tickPeriodic();
        CHECK_EQ(countFrames(h.core->takeOutboundFrames(),
                             F(wire::Func::StateEventV2)),
                 1);
    }

    // ---- outbound queue is bounded (100 Hz stream vs. no client) -----------
    {
        GatewayConfig cfg;
        cfg.outboundQueueCapacity = 8U;
        Harness h(cfg);
        for (int i = 0; i < 100; ++i) {
            h.core->tickPeriodic(); // each tick pushes one state event
        }
        auto frames = h.core->takeOutboundFrames();
        CHECK_EQ(frames.size(), static_cast<std::size_t>(8U)); // newest kept
        for (const auto& f : frames) {
            CHECK_EQ(f.funcId, F(wire::Func::StateEventV2));
        }
    }

    // ---- shutdown stop --------------------------------------------------------
    {
        Harness h;
        h.core->shutdownStop();
        CHECK(h.rov.called("stop"));
        CHECK(h.core->state().globalStopped);
    }

    TEST_MAIN_END;
}
