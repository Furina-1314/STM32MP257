#ifndef ROV_INTERNAL_SEQUENCE_ALLOCATOR_HPP
#define ROV_INTERNAL_SEQUENCE_ALLOCATOR_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace rov::internal {

class SequenceAllocator final {
public:
    static constexpr std::uint16_t SequenceCount = 10000U;

    explicit SequenceAllocator(std::uint16_t initial = 0) noexcept
        : next_(static_cast<std::uint16_t>(initial % SequenceCount))
    {
    }

    std::optional<std::uint16_t> select(
        const std::function<bool(std::uint16_t)>& unavailable) const
    {
        for (std::uint16_t offset = 0; offset < SequenceCount; ++offset) {
            const auto candidate = static_cast<std::uint16_t>(
                (static_cast<unsigned int>(next_) + offset) % SequenceCount);
            if (!unavailable(candidate)) {
                return candidate;
            }
        }
        return std::nullopt;
    }

    void commit(std::uint16_t used) noexcept
    {
        next_ = static_cast<std::uint16_t>(
            (static_cast<unsigned int>(used) + 1U) % SequenceCount);
    }

    std::uint16_t next() const noexcept
    {
        return next_;
    }

private:
    std::uint16_t next_;
};

} // namespace rov::internal

#endif // ROV_INTERNAL_SEQUENCE_ALLOCATOR_HPP
