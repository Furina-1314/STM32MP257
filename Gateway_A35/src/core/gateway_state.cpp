#include "core/gateway_state.hpp"

namespace gw {

bool GatewayState::apply(const wire::StateV2& next)
{
    if (next.safe && !next.attitudeStab) {
        return false; // illegal authoritative combination; keep previous
    }
    current_ = next;
    return true;
}

bool GatewayState::consumeChanged(wire::StateV2& out)
{
    const bool same = pushedOnce_
            && (lastPushed_.safe == current_.safe)
            && (lastPushed_.attitudeStab == current_.attitudeStab)
            && (lastPushed_.globalStopped == current_.globalStopped)
            && (lastPushed_.verticalStopped == current_.verticalStopped)
            && (lastPushed_.horizontalStopped == current_.horizontalStopped)
            && (lastPushed_.verticalSync == current_.verticalSync)
            && (lastPushed_.horizontalSync == current_.horizontalSync)
            && (lastPushed_.estop == current_.estop)
            && (lastPushed_.emergency == current_.emergency);
    if (same) {
        return false;
    }
    out = current_;
    lastPushed_ = current_;
    pushedOnce_ = true;
    return true;
}

void GatewayState::forcePush()
{
    pushedOnce_ = false;
}

} // namespace gw
