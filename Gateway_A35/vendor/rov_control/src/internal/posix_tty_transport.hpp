#ifndef ROV_INTERNAL_POSIX_TTY_TRANSPORT_HPP
#define ROV_INTERNAL_POSIX_TTY_TRANSPORT_HPP

#include "internal/transport.hpp"

#include <mutex>
#include <string>

namespace rov::internal {

class PosixTtyTransport final : public ITransport {
public:
    explicit PosixTtyTransport(std::string device);
    ~PosixTtyTransport() override;

    OpenResult open() override;
    void close() noexcept override;
    bool isOpen() const noexcept override;
    ReadResult readSome(std::chrono::milliseconds timeout) override;

protected:
    WriteResult writeSome(const char* data, std::size_t size) override;

private:
    std::string device_;
    mutable std::mutex stateMutex_;
    int fd_{-1};
};

} // namespace rov::internal

#endif // ROV_INTERNAL_POSIX_TTY_TRANSPORT_HPP
