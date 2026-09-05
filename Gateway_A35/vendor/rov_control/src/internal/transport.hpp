#ifndef ROV_INTERNAL_TRANSPORT_HPP
#define ROV_INTERNAL_TRANSPORT_HPP

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace rov::internal {

enum class IoStatus {
    Ok,
    Retry,
    Timeout,
    Disconnected,
    Error
};

struct OpenResult {
    IoStatus status{IoStatus::Error};
    int systemError{0};
    std::string detail;
};

struct ReadResult {
    IoStatus status{IoStatus::Error};
    std::string data;
    int systemError{0};
    std::string detail;
};

struct WriteResult {
    IoStatus status{IoStatus::Error};
    std::size_t bytesTransferred{0};
    int systemError{0};
    std::string detail;
};

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual OpenResult open() = 0;
    virtual void close() noexcept = 0;
    virtual bool isOpen() const noexcept = 0;
    virtual ReadResult readSome(std::chrono::milliseconds timeout) = 0;

    WriteResult writeAll(std::string_view data);

protected:
    virtual WriteResult writeSome(const char* data, std::size_t size) = 0;
};

} // namespace rov::internal

#endif // ROV_INTERNAL_TRANSPORT_HPP
