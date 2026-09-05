#include "wire/function_registry.hpp"

#include <algorithm>

namespace gw::wire {

const std::vector<FunctionEntry>& registry()
{
    static const std::vector<FunctionEntry> table = {
        // ---- system ----
        {Func::Ask, "ask", Direction::Request, true, kPriorityNormal},
        {Func::Ver, "ver", Direction::Request, true, kPriorityNormal},
        {Func::Status, "status", Direction::Request, true, kPriorityNormal},
        {Func::Help, "help", Direction::Request, true, kPriorityNormal},
        // ---- safety (Stop/Estop/Emergency all zero the six thrusters;
        //      identical execution, differing priority/alarm/log only) ----
        {Func::StopAll, "stop all", Direction::Request, true, kPriorityStopMove},
        {Func::Emergency, "emergency", Direction::Request, true, kPriorityEmergency},
        {Func::Estop, "estop", Direction::Request, true, kPriorityEstop},
        // ---- stop/move three-level enable latches ----
        {Func::MoveAll, "move all", Direction::Request, true, kPriorityStopMove},
        {Func::StopVertical, "stop vertical", Direction::Request, true, kPriorityStopMove},
        {Func::MoveVertical, "move vertical", Direction::Request, true, kPriorityStopMove},
        {Func::StopHorizontal, "stop horizontal", Direction::Request, true, kPriorityStopMove},
        {Func::MoveHorizontal, "move horizontal", Direction::Request, true, kPriorityStopMove},
        // ---- modes ----
        {Func::SafeOn, "safe on", Direction::Request, true, kPriorityNormal},
        {Func::SafeOff, "safe off", Direction::Request, true, kPriorityNormal},
        {Func::HorizontalOn, "horizontal on", Direction::Request, true, kPriorityNormal},
        {Func::HorizontalOff, "horizontal off", Direction::Request, true, kPriorityNormal},
        {Func::VerticalSyncOn, "vertical synchronization on", Direction::Request, true, kPriorityNormal},
        {Func::VerticalSyncOff, "vertical synchronization off", Direction::Request, true, kPriorityNormal},
        {Func::HorizontalSyncOn, "horizontal synchronization on", Direction::Request, true, kPriorityNormal},
        {Func::HorizontalSyncOff, "horizontal synchronization off", Direction::Request, true, kPriorityNormal},
        // ---- servo ----
        {Func::ServoSet, "set servo", Direction::Request, true, kPriorityNormal},
        {Func::ServoSetAll, "set servo all", Direction::Request, true, kPriorityNormal},
        {Func::ServoMid, "set servo mid", Direction::Request, true, kPriorityNormal},
        {Func::ServoGet, "get servo", Direction::Request, true, kPriorityNormal},
        {Func::ServoGetAll, "get servo all", Direction::Request, true, kPriorityNormal},
        // ---- propeller ----
        {Func::PropellerSet, "set propeller", Direction::Request, true, kPriorityNormal},
        {Func::PropellerSetAll, "set propeller all", Direction::Request, true, kPriorityNormal},
        {Func::PropellerStop, "set propeller stop", Direction::Request, true, kPriorityNormal},
        {Func::PropellerGetBase, "get propeller base", Direction::Request, true, kPriorityNormal},
        {Func::PropellerGetReal, "get propeller real", Direction::Request, true, kPriorityNormal},
        // ---- unified base ----
        {Func::BaseValue, "base value", Direction::Request, true, kPriorityNormal},
        {Func::BaseValueVH, "base value vh", Direction::Request, true, kPriorityNormal},
        // ---- sensors ----
        {Func::SensorMpu, "sensor mpu", Direction::Request, true, kPriorityNormal},
        {Func::SensorDyp, "sensor dyp", Direction::Request, true, kPriorityNormal},
        {Func::SensorAll, "sensor all", Direction::Request, true, kPriorityNormal},
        // ---- link ----
        {Func::Heartbeat, "heartbeat", Direction::Request, false, kPriorityNormal},
        // ---- A35 -> Windows ----
        {Func::SensorSummary, "sensor summary", Direction::Telemetry, false, kPriorityNormal},
        {Func::Ack, "ack", Direction::Response, false, kPriorityNormal},
        {Func::StateEvent, "state event", Direction::Event, false, kPriorityNormal},
        {Func::AlarmEvent, "alarm event", Direction::Event, false, kPriorityNormal},
        {Func::StateEventV2, "state event v2", Direction::Event, false, kPriorityNormal},
    };
    return table;
}

const FunctionEntry* findFunc(std::uint16_t funcId)
{
    const auto& table = registry();
    const auto it = std::find_if(
            table.begin(), table.end(),
            [funcId](const FunctionEntry& e) {
                return static_cast<std::uint16_t>(e.funcId) == funcId;
            });
    return (it == table.end()) ? nullptr : &(*it);
}

} // namespace gw::wire
