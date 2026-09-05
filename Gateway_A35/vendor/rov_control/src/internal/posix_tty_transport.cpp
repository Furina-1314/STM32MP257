#include "internal/posix_tty_transport.hpp"

#include <cerrno>
#include <cstring>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace rov::internal {

PosixTtyTransport::PosixTtyTransport(std::string device)
    : device_(std::move(device))
{
}

PosixTtyTransport::~PosixTtyTransport()
{
    close();
}

OpenResult PosixTtyTransport::open()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (fd_ >= 0) {
        return {IoStatus::Ok, 0, {}};
    }

#if defined(__linux__)
    const int fd = ::open(device_.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        return {IoStatus::Error, errno,
                "open(" + device_ + "): " + std::strerror(errno)};
    }

    termios settings{};
    if (::tcgetattr(fd, &settings) != 0) {
        const int savedError = errno;
        ::close(fd);
        return {IoStatus::Error, savedError,
                "tcgetattr(" + device_ + "): " +
                    std::strerror(savedError)};
    }

    // RPMsg TTY is not a physical UART. Preserve its existing speed fields,
    // while disabling all line discipline transformations that can echo,
    // buffer, or rewrite the ASCII protocol.
    ::cfmakeraw(&settings);
    settings.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHONL | ICANON));
    settings.c_oflag &= static_cast<tcflag_t>(~OPOST);
    settings.c_iflag &= static_cast<tcflag_t>(
        ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL |
          IXON));
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;

    if (::tcsetattr(fd, TCSANOW, &settings) != 0) {
        const int savedError = errno;
        ::close(fd);
        return {IoStatus::Error, savedError,
                "tcsetattr(" + device_ + "): " +
                    std::strerror(savedError)};
    }

    fd_ = fd;
    return {IoStatus::Ok, 0, {}};
#else
    return {IoStatus::Error, ENOTSUP,
            "PosixTtyTransport is available only on Linux"};
#endif
}

void PosixTtyTransport::close() noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex_);
#if defined(__linux__)
    if (fd_ >= 0) {
        (void)::close(fd_);
    }
#endif
    fd_ = -1;
}

bool PosixTtyTransport::isOpen() const noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return fd_ >= 0;
}

ReadResult PosixTtyTransport::readSome(std::chrono::milliseconds timeout)
{
#if defined(__linux__)
    int fd;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        fd = fd_;
    }
    if (fd < 0) {
        return {IoStatus::Disconnected, {}, 0, "transport is closed"};
    }

    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return {IoStatus::Timeout, {}, 0, {}};
        }
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline - now);
        const int pollTimeout = static_cast<int>(remaining.count() > 0
            ? remaining.count() : 1);
        const int pollResult = ::poll(&descriptor, 1, pollTimeout);
        if (pollResult == 0) {
            return {IoStatus::Timeout, {}, 0, {}};
        }
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {IoStatus::Error, {}, errno,
                    std::string("poll: ") + std::strerror(errno)};
        }
        if ((descriptor.revents & (POLLHUP | POLLNVAL)) != 0) {
            return {IoStatus::Disconnected, {}, 0,
                    "RPMsg TTY disconnected"};
        }
        if ((descriptor.revents & POLLERR) != 0) {
            return {IoStatus::Error, {}, EIO, "RPMsg TTY poll error"};
        }
        if ((descriptor.revents & POLLIN) != 0) {
            char buffer[512];
            const ssize_t count = ::read(fd, buffer, sizeof(buffer));
            if (count > 0) {
                return {IoStatus::Ok,
                        std::string(buffer, static_cast<std::size_t>(count)),
                        0, {}};
            }
            if (count == 0) {
                return {IoStatus::Disconnected, {}, 0,
                        "RPMsg TTY reached EOF"};
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return {IoStatus::Error, {}, errno,
                    std::string("read: ") + std::strerror(errno)};
        }
    }
#else
    (void)timeout;
    return {IoStatus::Error, {}, ENOTSUP,
            "PosixTtyTransport is available only on Linux"};
#endif
}

WriteResult PosixTtyTransport::writeSome(const char* data, std::size_t size)
{
#if defined(__linux__)
    int fd;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        fd = fd_;
    }
    if (fd < 0) {
        return {IoStatus::Disconnected, 0, 0, "transport is closed"};
    }

    const ssize_t count = ::write(fd, data, size);
    if (count > 0) {
        return {IoStatus::Ok, static_cast<std::size_t>(count), 0, {}};
    }
    if (count == 0) {
        return {IoStatus::Disconnected, 0, 0,
                "RPMsg TTY accepted zero bytes"};
    }
    if (errno == EINTR) {
        return {IoStatus::Retry, 0, errno, {}};
    }
    if (errno == EPIPE || errno == ENODEV || errno == ENXIO) {
        return {IoStatus::Disconnected, 0, errno,
                std::string("write: ") + std::strerror(errno)};
    }
    return {IoStatus::Error, 0, errno,
            std::string("write: ") + std::strerror(errno)};
#else
    (void)data;
    (void)size;
    return {IoStatus::Error, 0, ENOTSUP,
            "PosixTtyTransport is available only on Linux"};
#endif
}

} // namespace rov::internal
