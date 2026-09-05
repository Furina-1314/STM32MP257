#ifndef GW_CORE_GATEWAY_STATE_HPP
#define GW_CORE_GATEWAY_STATE_HPP

#include "wire/payload_codec.hpp"

namespace gw {

// Authoritative nine-bit state (StateEventV2). The gateway is the single
// authority; Windows mirrors it and must never observe the illegal
// combination safe=ON with attitudeStabilization=OFF.
class GatewayState
{
public:
    // Applies the next state. Rejects the illegal combination (returns
    // false and keeps the previous state); the caller logs/alarms, because
    // producing it would lock the Windows thruster panel with a
    // high-level alarm. Returns true when accepted (changed or not).
    bool apply(const wire::StateV2& next);

    const wire::StateV2& current() const { return current_; }

    // Full-state push bookkeeping: returns true and fills `out` when the
    // current state differs from the last pushed snapshot (or when forced).
    bool consumeChanged(wire::StateV2& out);
    void forcePush(); // invalidate the last-pushed snapshot

private:
    wire::StateV2 current_;
    bool pushedOnce_ = false;
    wire::StateV2 lastPushed_;
};

} // namespace gw

#endif // GW_CORE_GATEWAY_STATE_HPP
