#include "internal/rpmsg_client.hpp"

#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace rov::internal {
namespace {

RovFailure makeClientFailure(RovError error, std::string detail,
                             int systemError = 0)
{
    return {error, ErrorOrigin::Client, std::move(detail), {}, systemError};
}

RovError transportErrorCode(IoStatus status)
{
    return status == IoStatus::Disconnected
        ? RovError::Disconnected : RovError::TransportIo;
}

} // namespace

RpmsgClient::RpmsgClient(std::unique_ptr<ITransport> transport,
                         std::chrono::milliseconds defaultTimeout,
                         std::chrono::milliseconds quarantineDuration,
                         std::uint16_t initialSequence)
    : transport_(std::move(transport)),
      defaultTimeout_(defaultTimeout),
      quarantineDuration_(quarantineDuration),
      sequenceAllocator_(initialSequence)
{
}

RpmsgClient::~RpmsgClient()
{
    close();
}

RovResult<void> RpmsgClient::open()
{
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    if (running_.load()) {
        return RovResult<void>::success();
    }
    if (!transport_) {
        return RovResult<void>::fail(
            makeClientFailure(RovError::TransportIo,
                              "transport is not configured"));
    }

    const OpenResult opened = transport_->open();
    if (opened.status != IoStatus::Ok) {
        return RovResult<void>::fail(transportFailure(opened));
    }

    receiveBuffer_.clear();
    running_.store(true);
    try {
        readerThread_ = std::thread(&RpmsgClient::readerLoop, this);
    } catch (...) {
        running_.store(false);
        transport_->close();
        return RovResult<void>::fail(
            makeClientFailure(RovError::TransportIo,
                              "failed to create RPMsg reader thread"));
    }
    return RovResult<void>::success();
}

void RpmsgClient::close() noexcept
{
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    const bool wasRunning = running_.exchange(false);
    if (wasRunning) {
        failAllPending(makeClientFailure(
            RovError::Disconnected, "RPMsg client closed"));
    }
    if (readerThread_.joinable()) {
        readerThread_.join();
    }
    {
        // Do not close the fd while another thread is inside a complete
        // command write. Pending waiters were already failed above.
        std::lock_guard<std::mutex> writeLock(writeMutex_);
        if (transport_) {
            transport_->close();
        }
    }
}

bool RpmsgClient::isOpen() const noexcept
{
    return running_.load() && transport_ && transport_->isOpen();
}

RovResult<ClientResponse> RpmsgClient::request(
    const std::string& payload,
    ExpectedResponseKind expected,
    std::chrono::milliseconds timeout)
{
    if (timeout.count() <= 0) {
        timeout = defaultTimeout_;
    }
    if (!isOpen()) {
        return RovResult<ClientResponse>::fail(
            makeClientFailure(RovError::Disconnected,
                              "RPMsg client is not open"));
    }

    std::shared_ptr<PendingRequest> pending;
    std::future<RovResult<ClientResponse>> future;
    std::uint16_t sequence = 0;
    const auto deadline = Clock::now() + timeout;

    {
        std::lock_guard<std::mutex> writeLock(writeMutex_);
        if (!isOpen()) {
            return RovResult<ClientResponse>::fail(
                makeClientFailure(RovError::Disconnected,
                                  "RPMsg client disconnected before write"));
        }

        {
            std::lock_guard<std::mutex> pendingLock(pendingMutex_);
            const auto now = Clock::now();
            pruneQuarantineLocked(now);
            const auto selected = sequenceAllocator_.select(
                [this](std::uint16_t candidate) {
                    return pending_.find(candidate) != pending_.end() ||
                           quarantine_.find(candidate) != quarantine_.end();
                });
            if (!selected) {
                return RovResult<ClientResponse>::fail(
                    makeClientFailure(RovError::SequenceExhausted,
                                      "all 10000 sequences are unavailable"));
            }
            sequence = *selected;
            pending = std::make_shared<PendingRequest>();
            pending->expected = expected;
            pending->deadline = deadline;
            pending->commandName = payload;
            future = pending->completion.get_future();
            pending_.emplace(sequence, pending);
        }

        std::ostringstream wire;
        wire << std::setw(4) << std::setfill('0') << sequence
             << ' ' << payload << '\n';
        const WriteResult written = transport_->writeAll(wire.str());
        if (written.status != IoStatus::Ok) {
            {
                std::lock_guard<std::mutex> pendingLock(pendingMutex_);
                const auto found = pending_.find(sequence);
                if (found != pending_.end() && found->second == pending) {
                    pending_.erase(found);
                    quarantineLocked(sequence, Clock::now());
                }
            }
            return RovResult<ClientResponse>::fail(
                transportFailure(written), sequence);
        }
        sequenceAllocator_.commit(sequence);
    }

    if (future.wait_until(deadline) == std::future_status::ready) {
        return future.get();
    }

    {
        std::lock_guard<std::mutex> pendingLock(pendingMutex_);
        const auto found = pending_.find(sequence);
        if (found != pending_.end() && found->second == pending) {
            pending_.erase(found);
            quarantineLocked(sequence, Clock::now());
            return RovResult<ClientResponse>::fail(
                makeClientFailure(RovError::Timeout,
                                  "A35 request deadline reached"),
                sequence);
        }
    }

    // The reader removed this request while the timeout path was acquiring
    // the pending lock. It sets the promise before erasing, so get() is safe.
    return future.get();
}

void RpmsgClient::readerLoop()
{
    constexpr auto pollInterval = std::chrono::milliseconds(50);

    while (running_.load()) {
        ReadResult result = transport_->readSome(pollInterval);
        if (!running_.load()) {
            break;
        }
        if (result.status == IoStatus::Timeout ||
            result.status == IoStatus::Retry) {
            continue;
        }
        if (result.status == IoStatus::Ok) {
            consumeData(result.data);
            continue;
        }

        running_.store(false);
        failAllPending(makeClientFailure(
            transportErrorCode(result.status),
            result.detail.empty() ? "RPMsg read failed" : result.detail,
            result.systemError));
        break;
    }
}

void RpmsgClient::consumeData(const std::string& data)
{
    receiveBuffer_.append(data);
    for (;;) {
        const std::size_t newline = receiveBuffer_.find('\n');
        if (newline == std::string::npos) {
            break;
        }
        std::string line = receiveBuffer_.substr(0, newline);
        receiveBuffer_.erase(0, newline + 1U);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        dispatchLine(line);
    }

    // Current M33 replies are below 192 bytes. A missing LF must not allow an
    // unsolicited or corrupt stream to grow without bound.
    if (receiveBuffer_.size() > 4096U) {
        receiveBuffer_.clear();
    }
}

void RpmsgClient::dispatchLine(const std::string& line)
{
    const ParsedLine parsed = ResponseParser::parseLine(line);
    if ((parsed.lineKind != ParsedLineKind::Response &&
         parsed.lineKind != ParsedLineKind::Malformed) ||
        !parsed.hasSequence) {
        // Startup banners and future unsequenced events are diagnostics only.
        return;
    }

    std::shared_ptr<PendingRequest> pending;
    {
        std::lock_guard<std::mutex> pendingLock(pendingMutex_);
        const auto found = pending_.find(parsed.sequence);
        if (found == pending_.end()) {
            // A late or unknown sequenced response cannot complete a request.
            return;
        }
        pending = found->second;
        const RovResult<ClientResponse> result =
            parsed.lineKind == ParsedLineKind::Malformed
                ? RovResult<ClientResponse>::fail(
                    {RovError::ProtocolError, ErrorOrigin::Client,
                     parsed.detail, parsed.raw, 0}, parsed.sequence)
                : ResponseParser::validate(parsed, pending->expected);
        pending->completion.set_value(result);
        pending_.erase(found);
    }
}

void RpmsgClient::failAllPending(const RovFailure& failure) noexcept
{
    std::vector<std::pair<std::uint16_t, std::shared_ptr<PendingRequest>>>
        requests;
    {
        std::lock_guard<std::mutex> pendingLock(pendingMutex_);
        const auto now = Clock::now();
        for (auto& entry : pending_) {
            quarantineLocked(entry.first, now);
            requests.emplace_back(entry.first, entry.second);
        }
        pending_.clear();
    }
    for (auto& entry : requests) {
        try {
            entry.second->completion.set_value(
                RovResult<ClientResponse>::fail(failure, entry.first));
        } catch (...) {
            // close()/disconnect is noexcept; an abandoned future is harmless.
        }
    }
}

void RpmsgClient::quarantineLocked(std::uint16_t sequence,
                                   Clock::time_point now)
{
    quarantine_[sequence] = now + quarantineDuration_;
}

void RpmsgClient::pruneQuarantineLocked(Clock::time_point now)
{
    for (auto iterator = quarantine_.begin(); iterator != quarantine_.end();) {
        if (iterator->second <= now) {
            iterator = quarantine_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

RovFailure RpmsgClient::transportFailure(const OpenResult& result)
{
    return makeClientFailure(transportErrorCode(result.status),
                             result.detail, result.systemError);
}

RovFailure RpmsgClient::transportFailure(const WriteResult& result)
{
    return makeClientFailure(transportErrorCode(result.status),
                             result.detail.empty()
                                 ? "RPMsg write failed" : result.detail,
                             result.systemError);
}

} // namespace rov::internal
