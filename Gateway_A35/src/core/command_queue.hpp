#ifndef GW_CORE_COMMAND_QUEUE_HPP
#define GW_CORE_COMMAND_QUEUE_HPP

#include <cstddef>
#include <cstdint>
#include <deque>

#include "wire/wire_codec.hpp"

namespace gw {

// Inbound command queue with the protocol's scheduling guarantee:
//   Estop(0) > Emergency(1) > Stop/Move(2) > normal(5).
// The urgent deque is priority-sorted with stable (FIFO) order inside each
// priority level and is never dropped. The normal deque is bounded;
// on overflow the oldest normal command is dropped (keep-fresh), matching
// the Windows send-side policy.
class CommandQueue
{
public:
    // Returns false when a normal command was dropped due to overflow.
    bool push(const wire::WireFrame& frame, int priority, int normalCapacity);

    bool pop(wire::WireFrame& out);

    void clear(); // disconnect cleanup: pending commands are never replayed

    std::size_t size() const { return urgent_.size() + normal_.size(); }
    bool empty() const { return urgent_.empty() && normal_.empty(); }

private:
    struct Item
    {
        wire::WireFrame frame;
        int priority = 0;
        std::uint64_t order = 0;
    };
    std::deque<Item> urgent_;
    std::deque<Item> normal_;
    std::uint64_t nextOrder_ = 0;
};

} // namespace gw

#endif // GW_CORE_COMMAND_QUEUE_HPP
