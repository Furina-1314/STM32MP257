// gateway_probe - board-side verification client for rov_gateway (phase 4).
//
// Connects to the gateway (default 127.0.0.1:7000) and runs a non-dangerous
// scripted sequence: observe the connect-time StateEventV2 and the summary
// stream, issue read-only/system queries, then measure Estop round-trip
// latency while the sensor service is polling DYP. Prints PASS/FAIL lines
// plus latency statistics; exit code 0 iff everything passed.
//
//   usage: gateway_probe [port] [estop_count]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "net/net_platform.hpp"
#include "wire/function_registry.hpp"
#include "wire/payload_codec.hpp"
#include "wire/wire_codec.hpp"

using namespace gw; // net::/wire:: helpers below

namespace {

using Clock = std::chrono::steady_clock;

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

bool recvFrame(net::SocketHandle sock, wire::WireFrame& out, int timeoutMs)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    std::vector<std::uint8_t> buffer;
    for (;;) {
        if (buffer.size() < static_cast<std::size_t>(wire::kHeaderBytes)) {
            std::uint8_t byte = 0U;
            const int n = net::recvSome(sock, reinterpret_cast<char*>(&byte), 1);
            if (n <= 0) {
                return false;
            }
            buffer.push_back(byte);
            continue;
        }
        std::uint16_t len = 0U;
        wire::getU16(buffer, 10U, len);
        if (buffer.size()
            < static_cast<std::size_t>(wire::kHeaderBytes) + len + 2U) {
            std::uint8_t byte = 0U;
            const int n = net::recvSome(sock, reinterpret_cast<char*>(&byte), 1);
            if (n <= 0) {
                return false;
            }
            buffer.push_back(byte);
            continue;
        }
        out.funcId = 0U;
        out.seq = 0U;
        out.flags = 0U;
        wire::getU16(buffer, 5U, out.funcId);
        wire::getU16(buffer, 7U, out.seq);
        out.flags = buffer[9];
        out.payload.assign(buffer.begin() + wire::kHeaderBytes,
                           buffer.begin() + wire::kHeaderBytes + len);
        return true;
    }
}

bool sendRequest(net::SocketHandle sock, wire::Func func, std::uint16_t seq,
                 const std::vector<std::uint8_t>& payload = {})
{
    const auto wire = wire::encodeFrame(static_cast<std::uint16_t>(func), seq,
                                        wire::kFlagNeedAck, payload, 4096);
    return net::sendAll(sock, reinterpret_cast<const char*>(wire.data()),
                        static_cast<int>(wire.size()));
}

} // namespace

int main(int argc, char** argv)
{
    const std::uint16_t port =
            (argc > 1) ? static_cast<std::uint16_t>(std::atoi(argv[1])) : 7000U;
    const int estopCount = (argc > 2) ? std::atoi(argv[2]) : 20;

    if (!net::initSockets()) {
        std::printf("FAIL: socket init\n");
        return 1;
    }

    net::SocketHandle sock = net::kInvalidSocket;
    if (!net::connectLocal(port, sock)) {
        std::printf("FAIL: connect 127.0.0.1:%u\n", port);
        return 1;
    }
    net::setNoDelay(sock);
    std::printf("connected to 127.0.0.1:%u\n", port);

    // 1) Connect-time full state push.
    {
        wire::WireFrame f;
        bool gotState = false;
        std::uint16_t mask = 0U;
        while (recvFrame(sock, f, 1500)) {
            if (static_cast<wire::Func>(f.funcId) == wire::Func::StateEventV2) {
                wire::getU16(f.payload, 1U, mask);
                gotState = true;
                break;
            }
        }
        check(gotState, "connect-time StateEventV2 received");
        if (gotState) {
            std::printf("  initial mask: 0x%04X (stab=%d vsync=%d hsync=%d "
                        "gstop=%d)\n",
                        mask, (mask & wire::kStateV2AttitudeStab) ? 1 : 0,
                        (mask & wire::kStateV2VerticalSync) ? 1 : 0,
                        (mask & wire::kStateV2HorizontalSync) ? 1 : 0,
                        (mask & wire::kStateV2GlobalStopped) ? 1 : 0);
            check((mask & wire::kStateV2AttitudeStab) != 0U,
                  "startup alignment: stabilization ON");
            check((mask & wire::kStateV2VerticalSync) != 0U,
                  "startup alignment: vertical sync ON");
            check((mask & wire::kStateV2HorizontalSync) != 0U,
                  "startup alignment: horizontal sync ON");
        }
    }

    // 2) Summary stream rate over 2 s (>= 20 Hz is healthy for this check;
    //    the nominal cadence is 100 Hz).
    {
        wire::WireFrame f;
        int summaries = 0;
        std::uint8_t lastMask = 0xFFU;
        float lastVoltage = -1.0F;
        const auto until = Clock::now() + std::chrono::milliseconds(2000);
        while ((Clock::now() < until) && recvFrame(sock, f, 500)) {
            if (static_cast<wire::Func>(f.funcId) == wire::Func::SensorSummary) {
                ++summaries;
                if (f.payload.size() == 45U) {
                    lastMask = f.payload[40];
                    wire::getF32(f.payload, 32U, lastVoltage);
                }
            }
        }
        std::printf("  summaries in 2s: %d (~%d Hz), validMask=0x%02X "
                    "voltage=%.3fV\n",
                    summaries, summaries / 2, lastMask, lastVoltage);
        check(summaries >= 40, "sensor summary stream >= 20 Hz");
        check(lastMask != 0xFFU, "summary payload is 45 bytes");
    }

    // 3) Read-only queries: expect ACK errCode 0.
    {
        struct Query
        {
            const char* name;
            wire::Func func;
            std::vector<std::uint8_t> payload;
        };
        const Query queries[] = {
                {"ask", wire::Func::Ask, {}},
                {"ver", wire::Func::Ver, {}},
                {"status", wire::Func::Status, {}},
                {"help", wire::Func::Help, {}},
                {"sensor mpu", wire::Func::SensorMpu, {}},
                {"sensor all", wire::Func::SensorAll, {}},
                {"get servo all", wire::Func::ServoGetAll, {}},
        };
        std::uint16_t seq = 0x0100U;
        for (const Query& q : queries) {
            ++seq;
            if (!sendRequest(sock, q.func, seq, q.payload)) {
                check(false, q.name);
                continue;
            }
            bool acked = false;
            wire::WireFrame f;
            while (recvFrame(sock, f, 3000)) {
                if ((static_cast<wire::Func>(f.funcId) == wire::Func::Ack)
                    && (f.seq == seq)) {
                    std::uint16_t err = 0xFFFFU;
                    wire::getU16(f.payload, 0U, err);
                    check(err == 0U, q.name);
                    acked = true;
                    break;
                }
            }
            if (!acked) {
                check(false, q.name);
            }
        }
    }

    // 4) sensor dyp: ACK expected (0 normally; busy/timeout are honest
    //    outcomes, reported but only hard failures count).
    {
        const std::uint16_t seq = 0x0200U;
        sendRequest(sock, wire::Func::SensorDyp, seq);
        wire::WireFrame f;
        bool acked = false;
        std::uint16_t err = 0xFFFFU;
        while (recvFrame(sock, f, 5000)) {
            if ((static_cast<wire::Func>(f.funcId) == wire::Func::Ack)
                && (f.seq == seq)) {
                wire::getU16(f.payload, 0U, err);
                acked = true;
                break;
            }
        }
        std::printf("  sensor dyp errCode=%u\n", err);
        check(acked && (err <= 5U), "sensor dyp answered (busy/timeout ok)");
    }

    // 5) Estop round-trip latency while the DYP poller is running.
    {
        std::vector<double> samples;
        samples.reserve(static_cast<std::size_t>(estopCount));
        std::uint16_t seq = 0x0300U;
        for (int i = 0; i < estopCount; ++i) {
            ++seq;
            const auto t0 = Clock::now();
            if (!sendRequest(sock, wire::Func::Estop, seq)) {
                check(false, "estop send");
                break;
            }
            wire::WireFrame f;
            while (recvFrame(sock, f, 3000)) {
                if ((static_cast<wire::Func>(f.funcId) == wire::Func::Ack)
                    && (f.seq == seq)) {
                    const double ms = std::chrono::duration<double>(
                                              Clock::now() - t0)
                                              .count()
                            * 1000.0;
                    samples.push_back(ms);
                    // Drain the burst of summary/state frames between rounds.
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        double min = -1.0;
        double max = -1.0;
        double sum = 0.0;
        for (const double s : samples) {
            if ((min < 0.0) || (s < min)) {
                min = s;
            }
            if (s > max) {
                max = s;
            }
            sum += s;
        }
        if (!samples.empty()) {
            std::printf("  estop latency over %zu rounds: min=%.1fms "
                        "avg=%.1fms max=%.1fms\n",
                        samples.size(), min, sum / samples.size(), max);
            check(max < 500.0, "estop ACK worst case < 500 ms");
        } else {
            check(false, "estop latency measurement");
        }
    }

    // 6) Final authoritative state must show globalStopped (estop path).
    {
        wire::WireFrame f;
        bool got = false;
        std::uint16_t mask = 0U;
        const auto until = Clock::now() + std::chrono::milliseconds(1500);
        while ((Clock::now() < until) && recvFrame(sock, f, 300)) {
            if (static_cast<wire::Func>(f.funcId) == wire::Func::StateEventV2) {
                wire::getU16(f.payload, 1U, mask);
                got = true;
            }
        }
        check(got && ((mask & wire::kStateV2GlobalStopped) != 0U)
                  && ((mask & wire::kStateV2Estop) != 0U),
              "final state: global stopped + estop latched");
    }

    net::closeSocket(sock);
    net::shutdownSockets();
    std::printf("%s: %d failure(s)\n", failures == 0 ? "PROBE PASS" : "PROBE FAIL",
                failures);
    return failures == 0 ? 0 : 1;
}
