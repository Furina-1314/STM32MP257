#include "core/command_queue.hpp"

#include <algorithm>

#include "wire/function_registry.hpp"

namespace gw {

bool CommandQueue::push(const wire::WireFrame& frame, int priority,
                        int normalCapacity)
{
    Item item;
    item.frame = frame;
    item.priority = priority;
    item.order = nextOrder_++;

    if (priority < wire::kPriorityNormal) {
        // Stable insert before the first strictly-lower-priority item so
        // Estop always precedes Emergency precedes Stop/Move, and FIFO
        // within one level. Urgent items are never dropped.
        const auto pos = std::find_if(
                urgent_.begin(), urgent_.end(),
                [&item](const Item& queued) {
                    return queued.priority > item.priority;
                });
        urgent_.insert(pos, item);
        return true;
    }

    normal_.push_back(item);
    if (static_cast<int>(normal_.size()) > normalCapacity) {
        normal_.pop_front(); // drop oldest normal command
        return false;
    }
    return true;
}

bool CommandQueue::pop(wire::WireFrame& out)
{
    if (!urgent_.empty()) {
        out = urgent_.front().frame;
        urgent_.pop_front();
        return true;
    }
    if (!normal_.empty()) {
        out = normal_.front().frame;
        normal_.pop_front();
        return true;
    }
    return false;
}

void CommandQueue::clear()
{
    urgent_.clear();
    normal_.clear();
}

} // namespace gw
