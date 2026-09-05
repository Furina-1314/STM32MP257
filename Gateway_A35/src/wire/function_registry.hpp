#ifndef GW_WIRE_FUNCTION_REGISTRY_HPP
#define GW_WIRE_FUNCTION_REGISTRY_HPP

#include <cstdint>
#include <vector>

namespace gw::wire {

// Function IDs mirror the Windows terminal WireConstants/FunctionRegistry
// one-to-one (41 entries: the doc's "42" is an off-by-one, see DECISIONS.md
// D-15). Values are final per WINDOWS_A35_INTERFACE.md section 3; new
// functions must use unallocated ID ranges and be agreed by both sides
// before appearing here.
enum class Func : std::uint16_t
{
    Ask = 0x0001,
    Ver = 0x0002,
    Status = 0x0003,
    Help = 0x0004,

    StopAll = 0x0010,
    Emergency = 0x0011,
    Estop = 0x0012,
    MoveAll = 0x0013,
    StopVertical = 0x0014,
    MoveVertical = 0x0015,
    StopHorizontal = 0x0016,
    MoveHorizontal = 0x0017,

    SafeOn = 0x0020,
    SafeOff = 0x0021,
    HorizontalOn = 0x0022,
    HorizontalOff = 0x0023,
    VerticalSyncOn = 0x0024,
    VerticalSyncOff = 0x0025,
    HorizontalSyncOn = 0x0026,
    HorizontalSyncOff = 0x0027,

    ServoSet = 0x0030,
    ServoSetAll = 0x0031,
    ServoMid = 0x0032,
    ServoGet = 0x0033,
    ServoGetAll = 0x0034,

    PropellerSet = 0x0040,
    PropellerSetAll = 0x0041,
    PropellerStop = 0x0042,
    PropellerGetBase = 0x0043,
    PropellerGetReal = 0x0044,

    BaseValue = 0x0050,     // deprecated; ACK unsupported
    BaseValueVH = 0x0051,

    SensorMpu = 0x0060,
    SensorDyp = 0x0061,
    SensorAll = 0x0062,

    Heartbeat = 0x00F0,

    SensorSummary = 0x0100,
    Ack = 0x0101,
    StateEvent = 0x0102,    // legacy; gateway never sends (DECISIONS D-08)
    AlarmEvent = 0x0103,
    StateEventV2 = 0x0104,
};

enum class Direction
{
    Request,   // Windows -> A35
    Response,  // A35 -> Windows (passive)
    Event,     // A35 -> Windows (unsolicited)
    Telemetry, // A35 -> Windows (periodic stream)
};

// Send priorities (smaller wins; urgent queue is priority-sorted and never
// dropped): Estop > Emergency > Stop/Move > normal control.
constexpr int kPriorityEstop = 0;
constexpr int kPriorityEmergency = 1;
constexpr int kPriorityStopMove = 2;
constexpr int kPriorityNormal = 5;

struct FunctionEntry
{
    Func funcId;
    const char* name;
    Direction direction;
    bool needsAck;
    int priority;
};

// The single authoritative function table. Exactly 42 entries.
const std::vector<FunctionEntry>& registry();

// Lookup by raw wire funcId; nullptr when unknown (caller answers ACK
// unsupported when NeedAck was set, and must never crash).
const FunctionEntry* findFunc(std::uint16_t funcId);

// ---- actuator id topology (wire ids are protocol-spec ids only) ----------

constexpr std::uint8_t kServoIdFirst = 0U;
constexpr std::uint8_t kServoIdLast = 9U;
constexpr std::uint8_t kVerticalIdFirst = 10U;
constexpr std::uint8_t kVerticalIdLast = 13U;
constexpr std::uint8_t kHorizontalIdFirst = 14U;
constexpr std::uint8_t kHorizontalIdLast = 15U;
constexpr std::uint8_t kIdBroadcast = 0xFFU; // "all" selector (mid/stop only)

constexpr bool isValidServoId(std::uint8_t id)
{
    // kServoIdFirst is 0 and id is unsigned: upper bound check suffices.
    return id <= kServoIdLast;
}

constexpr bool isValidThrusterId(std::uint8_t id)
{
    return (id >= kVerticalIdFirst) && (id <= kHorizontalIdLast);
}

constexpr bool isValidPropellerStopId(std::uint8_t id)
{
    return isValidThrusterId(id) || (id == kIdBroadcast);
}

} // namespace gw::wire

#endif // GW_WIRE_FUNCTION_REGISTRY_HPP
