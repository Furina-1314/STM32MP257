#ifndef ROV_ROV_ERROR_HPP
#define ROV_ROV_ERROR_HPP

#include <string>

namespace rov {

enum class RovError {
    None,
    BadArgument,
    BadCommand,
    Busy,
    NotReady,
    Timeout,
    Safety,
    Io,
    Unsupported,
    ProtocolError,
    Disconnected,
    TransportIo,
    SequenceExhausted
};

enum class ErrorOrigin {
    None,
    M33,
    Client
};

struct RovFailure {
    RovError code{RovError::None};
    ErrorOrigin origin{ErrorOrigin::None};
    std::string detail;
    std::string rawResponse;
    int systemError{0};
};

const char* toString(RovError error) noexcept;
const char* toString(ErrorOrigin origin) noexcept;

} // namespace rov

#endif // ROV_ROV_ERROR_HPP
