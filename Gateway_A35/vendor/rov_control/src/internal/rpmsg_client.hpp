#ifndef ROV_INTERNAL_RPMSG_CLIENT_HPP
#define ROV_INTERNAL_RPMSG_CLIENT_HPP

#include "internal/response_parser.hpp"
#include "internal/sequence_allocator.hpp"
#include "internal/transport.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace rov::internal {

class RpmsgClient final {
public:
    explicit RpmsgClient(
        std::unique_ptr<ITransport> transport,
        std::chrono::milliseconds defaultTimeout =
            std::chrono::milliseconds(1000),
        std::chrono::milliseconds quarantineDuration =
            std::chrono::milliseconds(5000),
        std::uint16_t initialSequence = 0);
    ~RpmsgClient();

    RpmsgClient(const RpmsgClient&) = delete;
    RpmsgClient& operator=(const RpmsgClient&) = delete;

    RovResult<void> open();
    void close() noexcept;
    bool isOpen() const noexcept;

    RovResult<ClientResponse> request(
        const std::string& payload,
        ExpectedResponseKind expected,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

private:
    struct PendingRequest {
        std::promise<RovResult<ClientResponse>> completion;
        ExpectedResponseKind expected{ExpectedResponseKind::Ack};
        std::chrono::steady_clock::time_point deadline;
        std::string commandName;
    };

    using Clock = std::chrono::steady_clock;

    void readerLoop();
    void consumeData(const std::string& data);
    void dispatchLine(const std::string& line);
    void failAllPending(const RovFailure& failure) noexcept;
    void quarantineLocked(std::uint16_t sequence, Clock::time_point now);
    void pruneQuarantineLocked(Clock::time_point now);
    static RovFailure transportFailure(const OpenResult& result);
    static RovFailure transportFailure(const WriteResult& result);

    std::unique_ptr<ITransport> transport_;
    const std::chrono::milliseconds defaultTimeout_;
    const std::chrono::milliseconds quarantineDuration_;
    SequenceAllocator sequenceAllocator_;

    mutable std::mutex lifecycleMutex_;
    std::mutex writeMutex_;
    std::mutex pendingMutex_;
    std::unordered_map<std::uint16_t, std::shared_ptr<PendingRequest>> pending_;
    std::unordered_map<std::uint16_t, Clock::time_point> quarantine_;
    std::atomic<bool> running_{false};
    std::thread readerThread_;
    std::string receiveBuffer_;
};

} // namespace rov::internal

#endif // ROV_INTERNAL_RPMSG_CLIENT_HPP
