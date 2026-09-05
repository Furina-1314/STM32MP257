// CommandQueue: priority order, stability, urgent never dropped, bounded
// normal queue drops oldest, clear.
#include "test_support.hpp"

#include "core/command_queue.hpp"
#include "wire/function_registry.hpp"

namespace {

gw::wire::WireFrame frameOf(std::uint16_t funcId, std::uint16_t seq)
{
    gw::wire::WireFrame f;
    f.funcId = funcId;
    f.seq = seq;
    f.flags = gw::wire::kFlagNeedAck;
    return f;
}

} // namespace

int main()
{
    using namespace gw;
    using wire::kPriorityEstop, wire::kPriorityEmergency,
        wire::kPriorityStopMove, wire::kPriorityNormal;

    // FIFO within one priority level.
    {
        CommandQueue q;
        CHECK(q.push(frameOf(0x0030U, 1U), kPriorityNormal, 256));
        CHECK(q.push(frameOf(0x0030U, 2U), kPriorityNormal, 256));
        CHECK(q.push(frameOf(0x0030U, 3U), kPriorityNormal, 256));
        wire::WireFrame out;
        CHECK(q.pop(out) && out.seq == 1U);
        CHECK(q.pop(out) && out.seq == 2U);
        CHECK(q.pop(out) && out.seq == 3U);
        CHECK(!q.pop(out));
    }

    // Priority order: a late estop jumps ahead of queued normal commands.
    {
        CommandQueue q;
        for (std::uint16_t i = 1; i <= 50U; ++i) {
            q.push(frameOf(0x0030U, i), kPriorityNormal, 256);
        }
        q.push(frameOf(0x0012U, 100U), kPriorityEstop, 256);
        q.push(frameOf(0x0011U, 101U), kPriorityEmergency, 256);
        q.push(frameOf(0x0010U, 102U), kPriorityStopMove, 256);
        wire::WireFrame out;
        CHECK(q.pop(out) && out.seq == 100U); // estop first
        CHECK(q.pop(out) && out.seq == 101U); // then emergency
        CHECK(q.pop(out) && out.seq == 102U); // then stop/move
        CHECK(q.pop(out) && out.seq == 1U);   // then normal FIFO
    }

    // Stability inside the urgent queue: estops keep their relative order
    // and always precede emergency, which precedes stop/move.
    {
        CommandQueue q;
        q.push(frameOf(0x0010U, 1U), kPriorityStopMove, 256);
        q.push(frameOf(0x0011U, 2U), kPriorityEmergency, 256);
        q.push(frameOf(0x0012U, 3U), kPriorityEstop, 256);
        q.push(frameOf(0x0012U, 4U), kPriorityEstop, 256);
        q.push(frameOf(0x0011U, 5U), kPriorityEmergency, 256);
        q.push(frameOf(0x0017U, 6U), kPriorityStopMove, 256);
        wire::WireFrame out;
        CHECK(q.pop(out) && out.seq == 3U);
        CHECK(q.pop(out) && out.seq == 4U);
        CHECK(q.pop(out) && out.seq == 2U);
        CHECK(q.pop(out) && out.seq == 5U);
        CHECK(q.pop(out) && out.seq == 1U);
        CHECK(q.pop(out) && out.seq == 6U);
        CHECK(!q.pop(out));
    }

    // Urgent items are never dropped, no matter the normal-queue pressure.
    {
        CommandQueue q;
        for (int i = 0; i < 5000; ++i) {
            q.push(frameOf(0x0012U, static_cast<std::uint16_t>(i)),
                   kPriorityEstop, 4);
        }
        CHECK_EQ(q.size(), static_cast<std::size_t>(5000U));
    }

    // Normal queue is bounded; the oldest command is dropped on overflow.
    {
        CommandQueue q;
        for (std::uint16_t i = 0; i < 10U; ++i) {
            q.push(frameOf(0x0030U, i), kPriorityNormal, 4);
        }
        CHECK_EQ(q.size(), static_cast<std::size_t>(4U));
        wire::WireFrame out;
        CHECK(q.pop(out) && out.seq == 6U); // 0..5 were dropped
        CHECK(q.pop(out) && out.seq == 7U);
        CHECK(q.pop(out) && out.seq == 8U);
        CHECK(q.pop(out) && out.seq == 9U);
        CHECK(!q.pop(out));
    }

    // clear() empties everything (disconnect: no replay).
    {
        CommandQueue q;
        q.push(frameOf(0x0030U, 1U), kPriorityNormal, 256);
        q.push(frameOf(0x0012U, 2U), kPriorityEstop, 256);
        CHECK(!q.empty());
        q.clear();
        CHECK(q.empty());
        wire::WireFrame out;
        CHECK(!q.pop(out));
    }

    TEST_MAIN_END;
}
