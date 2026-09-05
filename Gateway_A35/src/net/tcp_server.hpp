#ifndef GW_NET_TCP_SERVER_HPP
#define GW_NET_TCP_SERVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net/net_platform.hpp"
#include "wire/frame_accumulator.hpp"

namespace gw {

// Forward declaration avoids a core include here; the server only moves
// frames and byte buffers.
struct OutboundFrame;

enum class ClientPolicy
{
    Takeover, // default (mock A35 behavior): the new client wins, the old
              // connection is closed
    RejectNew // the active client keeps control; new connections are closed
};

struct TcpServerConfig
{
    std::uint16_t port = 7000U;
    std::string bindAddr = "0.0.0.0";
    ClientPolicy policy = ClientPolicy::Takeover;
    int maxPayload = wire::kMaxPayloadDefault;
    int recvBufferLimit = wire::kRecvBufferLimitDefault;
};

// Single-control-authority TCP server. One thread calls run()/runOnce();
// inbound frames are delivered through the frame sink, outbound frames are
// pushed with send(). Framing rules mirror the Windows terminal:
// bad magic/version/CRC resync silently; oversize/overflow count as loss of
// framing and drop the client.
class TcpServer
{
public:
    using FrameSink = std::function<void(const wire::WireFrame&)>;
    using Notify = std::function<void()>;

    TcpServer(TcpServerConfig config, FrameSink onFrame, Notify onConnected,
              Notify onDisconnected);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool start(); // create the listener
    void stop();  // close listener + client, drop callbacks state

    bool isListening() const { return listener_ != net::kInvalidSocket; }
    std::uint16_t boundPort() const { return boundPort_; }
    bool hasClient() const { return client_ != net::kInvalidSocket; }

    // Encodes and sends the frames to the active client. A hard send error
    // drops the client (disconnect path). Returns frames actually sent.
    std::size_t send(const std::vector<OutboundFrame>& frames);

    // One poll cycle: accept (per policy), read + deframe, detect loss of
    // framing and orderly/error disconnects. Returns true when the client
    // set changed (connect or disconnect).
    bool runOnce(int timeoutMs);

    // Drops the active client through the normal disconnect path (used by
    // the heartbeat timeout policy).
    void dropClient() { closeClient(true); }

    // Convenience loop for the production thread.
    void run();

    // Statistics for logs/tests.
    std::uint64_t resyncCount() const { return resyncCount_; }
    std::uint64_t oversizeCount() const { return oversizeCount_; }

private:
    void closeClient(bool notify);
    void handleAccepted();
    void handleClientReadable();
    void deliverFrames();

    TcpServerConfig config_;
    FrameSink onFrame_;
    Notify onConnected_;
    Notify onDisconnected_;

    net::SocketHandle listener_ = net::kInvalidSocket;
    net::SocketHandle client_ = net::kInvalidSocket;
    std::uint16_t boundPort_ = 0U;
    std::unique_ptr<wire::FrameAccumulator> accumulator_;
    std::uint64_t resyncCount_ = 0U;
    std::uint64_t oversizeCount_ = 0U;
};

} // namespace gw

#endif // GW_NET_TCP_SERVER_HPP
