#include "net/net_platform.hpp"

#include <csignal>
#include <cstring>

namespace gw::net {

#ifdef _WIN32

namespace {
struct WsaRefcount
{
    int count = 0;
};
WsaRefcount& wsaRef()
{
    static WsaRefcount ref;
    return ref;
}
} // namespace

bool initSockets()
{
    auto& ref = wsaRef();
    if (ref.count == 0) {
        WSADATA data;
        const int rc = WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0) {
            return false;
        }
    }
    ++ref.count;
    return true;
}

void shutdownSockets()
{
    auto& ref = wsaRef();
    if (ref.count > 0) {
        --ref.count;
        if (ref.count == 0) {
            WSACleanup();
        }
    }
}

#else // POSIX

bool initSockets()
{
    static bool sigpipeIgnored = false;
    if (!sigpipeIgnored) {
        std::signal(SIGPIPE, SIG_IGN); // send() reports EPIPE instead
        sigpipeIgnored = true;
    }
    return true;
}

void shutdownSockets() {}

#endif

void setNonBlocking(SocketHandle handle, bool nonBlocking)
{
#ifdef _WIN32
    u_long mode = nonBlocking ? 1UL : 0UL;
    ioctlsocket(handle, FIONBIO, &mode);
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    if (flags < 0) {
        return;
    }
    const int next = nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    fcntl(handle, F_SETFL, next);
#endif
}

void setNoDelay(SocketHandle handle)
{
    int one = 1;
    setsockopt(handle, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&one), sizeof(one));
}

bool createListener(const char* bindAddr, std::uint16_t port,
                    SocketHandle& outHandle, std::uint16_t& outPort)
{
    outHandle = kInvalidSocket;
    outPort = 0U;

    SocketHandle handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == kInvalidSocket) {
        return false;
    }
    int one = 1;
    setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if ((bindAddr == nullptr) || (std::strcmp(bindAddr, "0.0.0.0") == 0)) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        // Legacy inet_addr keeps this portable across old MinGW headers;
        // bind addresses are config-provided dotted quads.
        const std::uint32_t address = inet_addr(bindAddr);
        if (address == htonl(INADDR_NONE)) {
            closeSocket(handle);
            return false;
        }
        addr.sin_addr.s_addr = address;
    }

    if (bind(handle, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSocket(handle);
        return false;
    }
    if (listen(handle, 4) != 0) {
        closeSocket(handle);
        return false;
    }

    sockaddr_in bound;
    socklen_t boundLen = sizeof(bound);
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &boundLen)
        != 0) {
        closeSocket(handle);
        return false;
    }
    outPort = ntohs(bound.sin_port);

    setNonBlocking(handle, true);
    outHandle = handle;
    return true;
}

SocketHandle acceptClient(SocketHandle listener, std::uint32_t* peerIp)
{
    sockaddr_in peer;
    socklen_t peerLen = sizeof(peer);
    const SocketHandle client = accept(
            listener, reinterpret_cast<sockaddr*>(&peer), &peerLen);
    if (client == kInvalidSocket) {
        return kInvalidSocket;
    }
    if (peerIp != nullptr) {
        *peerIp = ntohl(peer.sin_addr.s_addr);
    }
    return client;
}

int recvSome(SocketHandle handle, char* buffer, int capacity)
{
    return static_cast<int>(recv(handle, buffer, capacity, 0));
}

bool sendAll(SocketHandle handle, const char* buffer, int size)
{
    int sent = 0;
    while (sent < size) {
#ifdef _WIN32
        const int n = static_cast<int>(
                send(handle, buffer + sent, size - sent, 0));
#else
        const int n = static_cast<int>(
                send(handle, buffer + sent, size - sent, MSG_NOSIGNAL));
#endif
        if (n <= 0) {
            if (n < 0) {
#ifdef _WIN32
                const int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    continue; // caller keeps the socket non-blocking-free
                }
#else
                if ((errno == EINTR) || (errno == EAGAIN)
                    || (errno == EWOULDBLOCK)) {
                    continue;
                }
#endif
            }
            return false;
        }
        sent += n;
    }
    return true;
}

void closeSocket(SocketHandle& handle)
{
    if (handle == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(handle);
#else
    ::close(handle);
#endif
    handle = kInvalidSocket;
}

bool connectLocal(std::uint16_t port, SocketHandle& outHandle)
{
    outHandle = kInvalidSocket;
    SocketHandle handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == kInvalidSocket) {
        return false;
    }
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(0x7F000001U); // 127.0.0.1
    if (connect(handle, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
        != 0) {
        closeSocket(handle);
        return false;
    }
    outHandle = handle;
    return true;
}

} // namespace gw::net
