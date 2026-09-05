#include "net/tcp_server.hpp"

#include <cstring>

#include "core/gateway_core.hpp" // OutboundFrame
#include "wire/wire_codec.hpp"

namespace gw {

TcpServer::TcpServer(TcpServerConfig config, FrameSink onFrame,
                     Notify onConnected, Notify onDisconnected)
    : config_(config)
    , onFrame_(std::move(onFrame))
    , onConnected_(std::move(onConnected))
    , onDisconnected_(std::move(onDisconnected))
{
    net::initSockets();
}

TcpServer::~TcpServer()
{
    stop();
}

bool TcpServer::start()
{
    if (isListening()) {
        return true;
    }
    net::SocketHandle handle = net::kInvalidSocket;
    std::uint16_t port = 0U;
    if (!net::createListener(config_.bindAddr.c_str(), config_.port, handle,
                             port)) {
        return false;
    }
    listener_ = handle;
    boundPort_ = port;
    return true;
}

void TcpServer::stop()
{
    closeClient(false);
    net::closeSocket(listener_);
}

void TcpServer::closeClient(bool notify)
{
    if (client_ != net::kInvalidSocket) {
        net::closeSocket(client_);
        accumulator_.reset();
        if (notify && onDisconnected_) {
            onDisconnected_();
        }
    }
}

void TcpServer::handleAccepted()
{
    for (;;) { // drain the accept backlog in this cycle
        net::SocketHandle accepted = net::acceptClient(listener_, nullptr);
        if (accepted == net::kInvalidSocket) {
            return;
        }
        if (client_ != net::kInvalidSocket) {
            if (config_.policy == ClientPolicy::RejectNew) {
                net::closeSocket(accepted);
                continue; // active client keeps control
            }
            closeClient(true); // takeover: old client loses the link
        }
        client_ = accepted;
        net::setNonBlocking(client_, true);
        net::setNoDelay(client_);
        accumulator_ = std::make_unique<wire::FrameAccumulator>(
                config_.maxPayload, config_.recvBufferLimit);
        if (onConnected_) {
            onConnected_();
        }
    }
}

void TcpServer::handleClientReadable()
{
    char buffer[4096];
    const int received = net::recvSome(client_, buffer, sizeof(buffer));
    if (received == 0) {
        closeClient(true); // orderly disconnect
        return;
    }
    if (received < 0) {
#ifdef _WIN32
        const int err = WSAGetLastError();
        if ((err == WSAEWOULDBLOCK) || (err == WSAEINTR)) {
            return;
        }
#else
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) {
            return;
        }
#endif
        closeClient(true); // hard receive error
        return;
    }
    accumulator_->feed(reinterpret_cast<const std::uint8_t*>(buffer),
                       static_cast<std::size_t>(received));
    deliverFrames();
}

void TcpServer::deliverFrames()
{
    for (;;) {
        const wire::NextResult result = accumulator_->next();
        if (result.error == wire::FrameError::Oversize) {
            ++oversizeCount_; // framing lost: drop the client (protocol 1)
            closeClient(true);
            return;
        }
        if (result.error == wire::FrameError::Overflow) {
            closeClient(true);
            return;
        }
        if (result.resyncError != wire::FrameError::None) {
            ++resyncCount_; // transient garbage: resynced, keep going
        }
        if (!result.hasFrame) {
            return;
        }
        if (onFrame_) {
            onFrame_(result.frame);
        }
    }
}

bool TcpServer::runOnce(int timeoutMs)
{
    bool clientSetChanged = false;
    if (!isListening()) {
        return false;
    }

    for (int spin = 0; spin < 2; ++spin) {
        fd_set readSet;
        fd_set errorSet;
        FD_ZERO(&readSet);
        FD_ZERO(&errorSet);
        FD_SET(listener_, &readSet);
        FD_SET(listener_, &errorSet);
        net::SocketHandle maxHandle = listener_;
        if (client_ != net::kInvalidSocket) {
            FD_SET(client_, &readSet);
            FD_SET(client_, &errorSet);
            if (client_ > maxHandle) {
                maxHandle = client_;
            }
        }

        timeval timeout;
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;
        const int ready = select(static_cast<int>(maxHandle) + 1, &readSet,
                                 nullptr, &errorSet, &timeout);
        if (ready <= 0) {
            return clientSetChanged; // timeout or spurious wake
        }

        if (FD_ISSET(listener_, &readSet)) {
            handleAccepted();
            clientSetChanged = true;
        }
        if ((client_ != net::kInvalidSocket)
            && (FD_ISSET(client_, &readSet) || FD_ISSET(client_, &errorSet))) {
            const bool hadClient = (client_ != net::kInvalidSocket);
            handleClientReadable();
            if (hadClient && (client_ == net::kInvalidSocket)) {
                clientSetChanged = true;
            }
        }
        if (spin == 0) {
            timeoutMs = 0; // second pass is non-blocking drain only
        }
    }
    return clientSetChanged;
}

void TcpServer::run()
{
    while (isListening()) {
        runOnce(10);
    }
}

std::size_t TcpServer::send(const std::vector<OutboundFrame>& frames)
{
    std::size_t sent = 0U;
    for (const OutboundFrame& frame : frames) {
        if (client_ == net::kInvalidSocket) {
            break;
        }
        const std::vector<std::uint8_t> wire = wire::encodeFrame(
                frame.funcId, frame.seq, frame.flags, frame.payload,
                config_.maxPayload);
        if (wire.empty()) {
            continue; // refused to encode (oversize); logged upstream
        }
        if (!net::sendAll(client_,
                          reinterpret_cast<const char*>(wire.data()),
                          static_cast<int>(wire.size()))) {
            closeClient(true); // dead peer: drop without killing the service
            break;
        }
        ++sent;
    }
    return sent;
}

} // namespace gw
