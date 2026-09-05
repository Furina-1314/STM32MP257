// GatewayState: illegal-combination invariant and change detection.
#include "test_support.hpp"

#include "core/gateway_state.hpp"

int main()
{
    using namespace gw;
    wire::StateV2 s;

    // Fresh state: all false, nothing pushed yet.
    GatewayState state;
    CHECK(!state.current().safe);
    CHECK(!state.current().attitudeStab);

    // Illegal combination rejected: safe without stabilization.
    wire::StateV2 illegal;
    illegal.safe = true;
    illegal.attitudeStab = false;
    CHECK(!state.apply(illegal));
    CHECK(!state.current().safe); // previous state kept

    // Legal: stabilization first, then safe.
    wire::StateV2 legal;
    legal.attitudeStab = true;
    legal.safe = true;
    CHECK(state.apply(legal));
    CHECK(state.current().safe && state.current().attitudeStab);

    // Change detection: first consume always emits (never pushed).
    wire::StateV2 snapshot;
    CHECK(state.consumeChanged(snapshot));
    CHECK(snapshot.safe && snapshot.attitudeStab);

    // Unchanged state: no event.
    CHECK(!state.consumeChanged(snapshot));

    // Single-bit change emits and resyncs the snapshot.
    wire::StateV2 next = legal;
    next.globalStopped = true;
    CHECK(state.apply(next));
    CHECK(state.consumeChanged(snapshot));
    CHECK(snapshot.globalStopped);
    CHECK(!state.consumeChanged(snapshot));

    // Every bit participates in change detection.
    const struct
    {
        const char* name;
        void (*flip)(wire::StateV2&);
    } bits[] = {
        {"safe", [](wire::StateV2& s) { s.safe = !s.safe; }},
        {"attitudeStab", [](wire::StateV2& s) { s.attitudeStab = !s.attitudeStab; }},
        {"globalStopped", [](wire::StateV2& s) { s.globalStopped = !s.globalStopped; }},
        {"verticalStopped", [](wire::StateV2& s) { s.verticalStopped = !s.verticalStopped; }},
        {"horizontalStopped", [](wire::StateV2& s) { s.horizontalStopped = !s.horizontalStopped; }},
        {"verticalSync", [](wire::StateV2& s) { s.verticalSync = !s.verticalSync; }},
        {"horizontalSync", [](wire::StateV2& s) { s.horizontalSync = !s.horizontalSync; }},
        {"estop", [](wire::StateV2& s) { s.estop = !s.estop; }},
        {"emergency", [](wire::StateV2& s) { s.emergency = !s.emergency; }},
    };
    wire::StateV2 base = snapshot;
    for (const auto& bit : bits) {
        wire::StateV2 toggled = base;
        bit.flip(toggled);
        if (toggled.safe && !toggled.attitudeStab) {
            continue; // would be illegal; covered above
        }
        CHECK(state.apply(toggled));
        wire::StateV2 out;
        CHECK(state.consumeChanged(out));
    }

    // forcePush makes the next consume emit even without a change (used on
    // client connect and 1Hz refresh).
    state.forcePush();
    wire::StateV2 out;
    CHECK(state.consumeChanged(out));
    CHECK(!state.consumeChanged(out));

    TEST_MAIN_END;
}
