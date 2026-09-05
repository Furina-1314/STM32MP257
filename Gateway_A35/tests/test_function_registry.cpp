// Function registry: exactly 41 entries (the Windows terminal's actual
// FunctionRegistry size; the interface doc's "42" is off by one - see
// DECISIONS.md D-15), unique ids, Windows-aligned values,
// needsAck/priority invariants.
#include "test_support.hpp"

#include "wire/function_registry.hpp"

#include <set>

int main()
{
    using namespace gw::wire;
    using FV = std::uint16_t; // funcId value type

    const auto& table = registry();
    CHECK_EQ(table.size(), static_cast<std::size_t>(41U));

    // Unique funcIds only.
    {
        std::set<std::uint16_t> ids;
        for (const FunctionEntry& e : table) {
            CHECK(ids.insert(static_cast<std::uint16_t>(e.funcId)).second);
        }
        CHECK_EQ(ids.size(), static_cast<std::size_t>(41U));
    }

    // Spot-check funcId values against the Windows WireConstants table.
    CHECK_EQ(static_cast<FV>(Func::Ask), static_cast<FV>(0x0001));
    CHECK_EQ(static_cast<FV>(Func::Ver), static_cast<FV>(0x0002));
    CHECK_EQ(static_cast<FV>(Func::Status), static_cast<FV>(0x0003));
    CHECK_EQ(static_cast<FV>(Func::Help), static_cast<FV>(0x0004));
    CHECK_EQ(static_cast<FV>(Func::StopAll), static_cast<FV>(0x0010));
    CHECK_EQ(static_cast<FV>(Func::Emergency), static_cast<FV>(0x0011));
    CHECK_EQ(static_cast<FV>(Func::Estop), static_cast<FV>(0x0012));
    CHECK_EQ(static_cast<FV>(Func::MoveAll), static_cast<FV>(0x0013));
    CHECK_EQ(static_cast<FV>(Func::StopVertical), static_cast<FV>(0x0014));
    CHECK_EQ(static_cast<FV>(Func::MoveVertical), static_cast<FV>(0x0015));
    CHECK_EQ(static_cast<FV>(Func::StopHorizontal), static_cast<FV>(0x0016));
    CHECK_EQ(static_cast<FV>(Func::MoveHorizontal), static_cast<FV>(0x0017));
    CHECK_EQ(static_cast<FV>(Func::SafeOn), static_cast<FV>(0x0020));
    CHECK_EQ(static_cast<FV>(Func::SafeOff), static_cast<FV>(0x0021));
    CHECK_EQ(static_cast<FV>(Func::HorizontalOn), static_cast<FV>(0x0022));
    CHECK_EQ(static_cast<FV>(Func::HorizontalOff), static_cast<FV>(0x0023));
    CHECK_EQ(static_cast<FV>(Func::VerticalSyncOn), static_cast<FV>(0x0024));
    CHECK_EQ(static_cast<FV>(Func::VerticalSyncOff), static_cast<FV>(0x0025));
    CHECK_EQ(static_cast<FV>(Func::HorizontalSyncOn), static_cast<FV>(0x0026));
    CHECK_EQ(static_cast<FV>(Func::HorizontalSyncOff), static_cast<FV>(0x0027));
    CHECK_EQ(static_cast<FV>(Func::ServoSet), static_cast<FV>(0x0030));
    CHECK_EQ(static_cast<FV>(Func::ServoSetAll), static_cast<FV>(0x0031));
    CHECK_EQ(static_cast<FV>(Func::ServoMid), static_cast<FV>(0x0032));
    CHECK_EQ(static_cast<FV>(Func::ServoGet), static_cast<FV>(0x0033));
    CHECK_EQ(static_cast<FV>(Func::ServoGetAll), static_cast<FV>(0x0034));
    CHECK_EQ(static_cast<FV>(Func::PropellerSet), static_cast<FV>(0x0040));
    CHECK_EQ(static_cast<FV>(Func::PropellerSetAll), static_cast<FV>(0x0041));
    CHECK_EQ(static_cast<FV>(Func::PropellerStop), static_cast<FV>(0x0042));
    CHECK_EQ(static_cast<FV>(Func::PropellerGetBase), static_cast<FV>(0x0043));
    CHECK_EQ(static_cast<FV>(Func::PropellerGetReal), static_cast<FV>(0x0044));
    CHECK_EQ(static_cast<FV>(Func::BaseValue), static_cast<FV>(0x0050));
    CHECK_EQ(static_cast<FV>(Func::BaseValueVH), static_cast<FV>(0x0051));
    CHECK_EQ(static_cast<FV>(Func::SensorMpu), static_cast<FV>(0x0060));
    CHECK_EQ(static_cast<FV>(Func::SensorDyp), static_cast<FV>(0x0061));
    CHECK_EQ(static_cast<FV>(Func::SensorAll), static_cast<FV>(0x0062));
    CHECK_EQ(static_cast<FV>(Func::Heartbeat), static_cast<FV>(0x00F0));
    CHECK_EQ(static_cast<FV>(Func::SensorSummary), static_cast<FV>(0x0100));
    CHECK_EQ(static_cast<FV>(Func::Ack), static_cast<FV>(0x0101));
    CHECK_EQ(static_cast<FV>(Func::StateEvent), static_cast<FV>(0x0102));
    CHECK_EQ(static_cast<FV>(Func::AlarmEvent), static_cast<FV>(0x0103));
    CHECK_EQ(static_cast<FV>(Func::StateEventV2), static_cast<FV>(0x0104));

    // All Windows->A35 requests need ACK, except the heartbeat.
    {
        int requests = 0;
        for (const FunctionEntry& e : table) {
            if (e.direction == Direction::Request) {
                ++requests;
                if (e.funcId == Func::Heartbeat) {
                    CHECK(!e.needsAck);
                } else {
                    CHECK(e.needsAck);
                }
            } else {
                CHECK(!e.needsAck); // A35->Windows frames are never ACKed
            }
        }
        CHECK_EQ(requests, 36);
        // 36 requests + 5 A35->Windows frames (0100/0101/0102/0103/0104) = 41.
    }

    // Priority invariants: estop(0) < emergency(1) < stop/move(2) < normal(5).
    {
        CHECK_EQ(findFunc(0x0012U)->priority, kPriorityEstop);
        CHECK_EQ(findFunc(0x0011U)->priority, kPriorityEmergency);
        for (const std::uint16_t id :
             {0x0010U, 0x0013U, 0x0014U, 0x0015U, 0x0016U, 0x0017U}) {
            CHECK_EQ(findFunc(id)->priority, kPriorityStopMove);
        }
        CHECK_EQ(findFunc(0x0030U)->priority, kPriorityNormal);
        CHECK_EQ(findFunc(0x0051U)->priority, kPriorityNormal);
        CHECK(kPriorityEstop < kPriorityEmergency);
        CHECK(kPriorityEmergency < kPriorityStopMove);
        CHECK(kPriorityStopMove < kPriorityNormal);
    }

    // Lookup: every entry found; unknown ids return nullptr (never crash).
    {
        for (const FunctionEntry& e : table) {
            const FunctionEntry* found =
                    findFunc(static_cast<std::uint16_t>(e.funcId));
            CHECK(found != nullptr);
            CHECK_EQ(found->name, e.name);
        }
        CHECK(findFunc(0x0099U) == nullptr);
        CHECK(findFunc(0x0000U) == nullptr);
        CHECK(findFunc(0xFFFFU) == nullptr);
        CHECK(findFunc(0x0105U) == nullptr); // first free id after the table
    }

    // Names are stable strings (used in logs).
    {
        CHECK(std::string(findFunc(0x0012U)->name) == "estop");
        CHECK(std::string(findFunc(0x0051U)->name) == "base value vh");
        CHECK(std::string(findFunc(0x0104U)->name) == "state event v2");
    }

    // Actuator id topology helpers.
    {
        CHECK(isValidServoId(0U));
        CHECK(isValidServoId(9U));
        CHECK(!isValidServoId(10U));
        CHECK(!isValidServoId(0xFFU));
        CHECK(isValidThrusterId(10U));
        CHECK(isValidThrusterId(13U));
        CHECK(isValidThrusterId(14U));
        CHECK(isValidThrusterId(15U));
        CHECK(!isValidThrusterId(9U));
        CHECK(!isValidThrusterId(16U));
        CHECK(isValidPropellerStopId(0xFFU));
        CHECK(isValidPropellerStopId(12U));
        CHECK(!isValidPropellerStopId(9U));
    }

    TEST_MAIN_END;
}
