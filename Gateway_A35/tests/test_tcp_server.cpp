// Real-socket loopback test of the single-client TCP server: framing over
// TCP, takeover/reject policies, disconnect cleanup, oversize handling,
// and an end-to-end estop round trip through GatewayCore.
#include "fake_rov_control.hpp"
#include "test_support.hpp"

#include "core/gateway_core.hpp"
#include "net/net_platform.hpp"
#include "net/tcp_server.hpp"
#include "wire/crc16.hpp"
#include "wire/function_registry.hpp"
#include "wire/wire_codec.hpp"

#include <cstring>
#include <memory>

using gw::net::SocketHandle;
using gw::net::kInvalidSocket;
using gw::net::initSockets;
using gw::net::shutdownSockets;
using gw::net::connectLocal;
using gw::net::recvSome;
using gw::net::sendAll;
using gw::net::closeSocket;

namespace {

int recvFrame(SocketHandle sock, gw::wire::WireFrame& out)
{
    // Reads one frame (header + payload + crc) byte-wise on a blocking
    // socket; returns 1 on success, -1 on error/EOF.
    std::vector<std::uint8_t> buffer;
    while (buffer.size() < static_cast<std::size_t>(gw::wire::kHeaderBytes)) {
        std::uint8_t byte = 0U;
        const int n = recvSome(sock, reinterpret_cast<char*>(&byte), 1);
        if (n <= 0) {
            return -1;
        }
        buffer.push_back(byte);
    }
    std::uint16_t len = 0U;
    gw::wire::getU16(buffer, 10U, len);
    while (buffer.size()
           < static_cast<std::size_t>(gw::wire::kHeaderBytes) + len + 2U) {
        std::uint8_t byte = 0U;
        const int n = recvSome(sock, reinterpret_cast<char*>(&byte), 1);
        if (n <= 0) {
            return -1;
        }
        buffer.push_back(byte);
    }
    out.funcId = 0U;
    out.seq = 0U;
    out.flags = 0U;
    gw::wire::getU16(buffer, 5U, out.funcId);
    gw::wire::getU16(buffer, 7U, out.seq);
    out.flags = buffer[9];
    out.payload.assign(buffer.begin() + gw::wire::kHeaderBytes,
                       buffer.begin() + gw::wire::kHeaderBytes + len);
    return 1;
}

} // namespace

int main()
{
    using namespace gw;
    using wire::Func;
    initSockets();

    // ---- end-to-end: connect, estop, ACK + StateEventV2, half-split feed --
    {
        FakeRovControl rov;
        GatewayCore core(rov);
        CHECK(core.alignStartupState());
        rov.calls.clear(); // alignment traffic must not pollute assertions

        int connects = 0;
        int disconnects = 0;
        TcpServerConfig cfg;
        cfg.port = 0U; // ephemeral
        TcpServer server(cfg,
                         [&core](const wire::WireFrame& f) {
                             core.submitRequest(f);
                             core.drainQueue();
                         },
                         [&core, &connects] {
                             ++connects;
                             core.onClientConnected();
                         },
                         [&core, &disconnects] {
                             ++disconnects;
                             core.onClientDisconnected();
                         });
        CHECK(server.start());
        CHECK(server.boundPort() != 0U);

        SocketHandle client = kInvalidSocket;
        CHECK(connectLocal(server.boundPort(), client));

        // Pump the server until the client is registered.
        for (int i = 0; (i < 50) && !server.hasClient(); ++i) {
            server.runOnce(20);
        }
        CHECK(server.hasClient());
        CHECK_EQ(connects, 1);

        // Server pushes a full state event on connect.
        server.send(core.takeOutboundFrames());
        wire::WireFrame pushed;
        CHECK(recvFrame(client, pushed) == 1);
        CHECK_EQ(pushed.funcId, static_cast<std::uint16_t>(Func::StateEventV2));

        // Estop request split across two TCP writes.
        const auto estopWire = wire::encodeFrame(
                static_cast<std::uint16_t>(Func::Estop), 0x4242U,
                wire::kFlagNeedAck, {}, 4096);
        CHECK(sendAll(client,
                           reinterpret_cast<const char*>(estopWire.data()),
                           static_cast<int>(estopWire.size() / 2)));
        server.runOnce(20); // half frame must not produce anything
        CHECK(rov.calls.empty());
        CHECK(sendAll(client,
                           reinterpret_cast<const char*>(estopWire.data()
                                                         + estopWire.size() / 2),
                           static_cast<int>(estopWire.size()
                                            - estopWire.size() / 2)));
        for (int i = 0; (i < 50) && rov.calls.empty(); ++i) {
            server.runOnce(20);
        }
        CHECK(rov.called("stop"));

        server.send(core.takeOutboundFrames());
        wire::WireFrame ack;
        CHECK(recvFrame(client, ack) == 1);
        CHECK_EQ(ack.funcId, static_cast<std::uint16_t>(Func::Ack));
        CHECK_EQ(ack.seq, 0x4242U); // Windows seq echoed verbatim
        std::uint16_t errCode = 1U;
        wire::getU16(ack.payload, 0U, errCode);
        CHECK_EQ(errCode, 0U);
        wire::WireFrame stateEvent;
        CHECK(recvFrame(client, stateEvent) == 1);
        CHECK_EQ(stateEvent.funcId,
                 static_cast<std::uint16_t>(Func::StateEventV2));
        std::uint16_t mask = 0U;
        wire::getU16(stateEvent.payload, 1U, mask);
        CHECK_EQ(mask & wire::kStateV2Estop, wire::kStateV2Estop);
        CHECK_EQ(mask & wire::kStateV2GlobalStopped,
                 wire::kStateV2GlobalStopped);

        // Disconnect is detected and cleans up through the core.
        closeSocket(client);
        for (int i = 0; (i < 50) && disconnects == 0; ++i) {
            server.runOnce(20);
        }
        CHECK_EQ(disconnects, 1);
        CHECK(!server.hasClient());
        CHECK(core.state().globalStopped); // stop_on_disconnect latched again
        server.stop();
    }

    // ---- takeover policy: the new client wins, the old link is closed ----
    {
        FakeRovControl rov;
        GatewayCore core(rov);
        core.alignStartupState();
        rov.calls.clear(); // alignment traffic must not pollute assertions
        TcpServerConfig cfg;
        cfg.port = 0U;
        int disconnects = 0;
        TcpServer server(cfg,
                         [&core](const wire::WireFrame& f) {
                             core.submitRequest(f);
                             core.drainQueue();
                         },
                         [&core] { core.onClientConnected(); },
                         [&core, &disconnects] {
                             ++disconnects;
                             core.onClientDisconnected();
                         });
        CHECK(server.start());

        SocketHandle first = kInvalidSocket;
        CHECK(connectLocal(server.boundPort(), first));
        for (int i = 0; (i < 50) && !server.hasClient(); ++i) {
            server.runOnce(20);
        }
        CHECK(server.hasClient());

        SocketHandle second = kInvalidSocket;
        CHECK(connectLocal(server.boundPort(), second));
        for (int i = 0; (i < 50) && (disconnects == 0); ++i) {
            server.runOnce(20);
        }
        CHECK_EQ(disconnects, 1); // old client was dropped
        CHECK(server.hasClient()); // new client owns the link
        // The first socket observes an orderly close.
        char probe = 0;
        int closedSeen = -1;
        for (int i = 0; i < 20; ++i) {
            closedSeen = recvSome(first, &probe, 1);
            if (closedSeen == 0) {
                break;
            }
        }
        CHECK_EQ(closedSeen, 0);
        closeSocket(first);
        closeSocket(second);
        server.stop();
    }

    // ---- reject policy: the active client keeps control -------------------
    {
        FakeRovControl rov;
        GatewayCore core(rov);
        TcpServerConfig cfg;
        cfg.port = 0U;
        cfg.policy = ClientPolicy::RejectNew;
        int connects = 0;
        int disconnects = 0;
        TcpServer server(cfg,
                         [&core](const wire::WireFrame& f) {
                             core.submitRequest(f);
                             core.drainQueue();
                         },
                         [&core, &connects] {
                             ++connects;
                             core.onClientConnected();
                         },
                         [&core, &disconnects] {
                             ++disconnects;
                             core.onClientDisconnected();
                         });
        CHECK(server.start());

        SocketHandle first = kInvalidSocket;
        CHECK(connectLocal(server.boundPort(), first));
        for (int i = 0; (i < 50) && connects < 1; ++i) {
            server.runOnce(20);
        }
        CHECK_EQ(connects, 1);

        SocketHandle second = kInvalidSocket;
        CHECK(connectLocal(server.boundPort(), second));
        server.runOnce(20);
        server.runOnce(20);
        CHECK_EQ(connects, 1); // no second connect notification
        CHECK_EQ(disconnects, 0); // the active client was not disturbed

        // The rejected socket sees an orderly close.
        char probe = 0;
        int closedSeen = -1;
        for (int i = 0; i < 20; ++i) {
            closedSeen = recvSome(second, &probe, 1);
            if (closedSeen == 0) {
                break;
            }
        }
        CHECK_EQ(closedSeen, 0);

        closeSocket(first);
        closeSocket(second);
        server.stop();
    }

    // ---- framing robustness: garbage then good frame; oversize drops ------
    {
        FakeRovControl rov;
        GatewayCore core(rov);
        core.alignStartupState();
        rov.calls.clear(); // alignment traffic must not pollute assertions
        core.takeOutboundFrames();
        TcpServerConfig cfg;
        cfg.port = 0U;
        int disconnects = 0;
        TcpServer server(cfg,
                         [&core](const wire::WireFrame& f) {
                             core.submitRequest(f);
                             core.drainQueue();
                         },
                         [&core] { core.onClientConnected(); },
                         [&core, &disconnects] {
                             ++disconnects;
                             core.onClientDisconnected();
                         });
        CHECK(server.start());

        SocketHandle client = kInvalidSocket;
        CHECK(connectLocal(server.boundPort(), client));
        for (int i = 0; (i < 50) && !server.hasClient(); ++i) {
            server.runOnce(20);
        }
        CHECK(server.hasClient());
        server.send(core.takeOutboundFrames()); // connect push to client
        wire::WireFrame connectPush;
        CHECK(recvFrame(client, connectPush) == 1); // consume it
        CHECK_EQ(connectPush.funcId,
                 static_cast<std::uint16_t>(Func::StateEventV2));

        // Three garbage bytes followed by a valid ask frame: resync recovers.
        const auto askWire = wire::encodeFrame(
                static_cast<std::uint16_t>(Func::Ask), 7U,
                wire::kFlagNeedAck, {}, 4096);
        std::vector<std::uint8_t> stream = {0x00, 0x11, 0x22};
        stream.insert(stream.end(), askWire.begin(), askWire.end());
        CHECK(sendAll(client, reinterpret_cast<const char*>(stream.data()),
                           static_cast<int>(stream.size())));
        server.runOnce(20);
        server.runOnce(20);
        server.runOnce(20);
        server.send(core.takeOutboundFrames());
        wire::WireFrame ack;
        CHECK(recvFrame(client, ack) == 1);
        CHECK_EQ(ack.funcId, static_cast<std::uint16_t>(Func::Ack));
        CHECK_EQ(ack.seq, 7U);
        CHECK(server.resyncCount() >= 1U);;

        // Oversize frame: the client is dropped (framing lost).
        std::vector<std::uint8_t> oversize;
        wire::putU32(oversize, wire::kMagic);
        wire::putU8(oversize, wire::kVersion);
        wire::putU16(oversize, static_cast<std::uint16_t>(Func::Ask));
        wire::putU16(oversize, 1U);
        wire::putU8(oversize, wire::kFlagNeedAck);
        wire::putU16(oversize, 5000U); // > maxPayload 4096
        for (int i = 0; i < 32; ++i) {
            oversize.push_back(0xAA);
        }
        wire::putU16(oversize, wire::crc16(oversize));
        CHECK(sendAll(client,
                           reinterpret_cast<const char*>(oversize.data()),
                           static_cast<int>(oversize.size())));
        for (int i = 0; (i < 50) && (disconnects == 0); ++i) {
            server.runOnce(20);
        }
        CHECK_EQ(disconnects, 1);
        CHECK(!server.hasClient());
        closeSocket(client);
        server.stop();
    }

    shutdownSockets();
    TEST_MAIN_END;
}
