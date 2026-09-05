#ifndef ROV_ROV_RESULT_HPP
#define ROV_ROV_RESULT_HPP

#include "rov/rov_error.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace rov {

template <typename T>
struct RovResult {
    std::optional<T> value;
    RovFailure failure;
    std::optional<std::uint16_t> sequence;

    explicit operator bool() const noexcept
    {
        return failure.code == RovError::None;
    }

    static RovResult success(T result,
                             std::optional<std::uint16_t> seq = std::nullopt)
    {
        RovResult output;
        output.value = std::move(result);
        output.sequence = seq;
        return output;
    }

    static RovResult fail(RovFailure error,
                          std::optional<std::uint16_t> seq = std::nullopt)
    {
        RovResult output;
        output.failure = std::move(error);
        output.sequence = seq;
        return output;
    }
};

template <>
struct RovResult<void> {
    RovFailure failure;
    std::optional<std::uint16_t> sequence;

    explicit operator bool() const noexcept
    {
        return failure.code == RovError::None;
    }

    static RovResult success(
        std::optional<std::uint16_t> seq = std::nullopt)
    {
        RovResult output;
        output.sequence = seq;
        return output;
    }

    static RovResult fail(RovFailure error,
                          std::optional<std::uint16_t> seq = std::nullopt)
    {
        RovResult output;
        output.failure = std::move(error);
        output.sequence = seq;
        return output;
    }
};

} // namespace rov

#endif // ROV_ROV_RESULT_HPP
