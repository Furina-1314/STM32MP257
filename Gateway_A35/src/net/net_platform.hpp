#ifndef GW_NET_NET_PLATFORM_HPP
#define GW_NET_NET_PLATFORM_HPP

// Thin portable socket layer over WinSock2 / BSD sockets so the gateway and
// its host tests build and run on both Windows and OpenSTLinux.

#include <cstdint>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 // inet_pton and friends
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#else // POSIX

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#endif

namespace gw::net {

#ifdef _WIN32
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

// Process-wide socket stack init/shutdown (WinSock needs an explicit
// startup; POSIX only ignores SIGPIPE). Refcounted; safe to nest.
bool initSockets();
void shutdownSockets();

// Creates a listening TCP socket bound to bindAddr:port. port 0 picks an
// ephemeral port (tests). The socket is non-blocking for accept polling.
bool createListener(const char* bindAddr, std::uint16_t port,
                    SocketHandle& outHandle, std::uint16_t& outPort);

SocketHandle acceptClient(SocketHandle listener, std::uint32_t* peerIp);

// Reads whatever is available; returns bytes read, 0 on orderly close,
// -1 on error/would-block (callers treat EWOULDBLOCK as "nothing yet").
int recvSome(SocketHandle handle, char* buffer, int capacity);

// Sends the whole buffer with retries on partial writes. Returns false on
// a hard error. On POSIX uses MSG_NOSIGNAL (no SIGPIPE).
bool sendAll(SocketHandle handle, const char* buffer, int size);

void closeSocket(SocketHandle& handle);

void setNonBlocking(SocketHandle handle, bool nonBlocking);

// Enables TCP_NODELAY for low-latency control traffic.
void setNoDelay(SocketHandle handle);

// Test helper: connect a client socket to 127.0.0.1:port (blocking).
bool connectLocal(std::uint16_t port, SocketHandle& outHandle);

} // namespace gw::net

#endif // GW_NET_NET_PLATFORM_HPP
