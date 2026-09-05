#include "rov/rov_control.hpp"

#include "internal/posix_tty_transport.hpp"
#include "internal/rov_control_test_access.hpp"
#include "internal/rpmsg_client.hpp"

#include <chrono>
#include <string_view>
#include <utility>

namespace rov {
namespace {

RovFailure badArgument(std::string detail)
{
    return {RovError::BadArgument, ErrorOrigin::Client,
            std::move(detail), {}, 0};
}

RovFailure badResponse(std::string detail, std::string raw)
{
    return {RovError::ProtocolError, ErrorOrigin::Client,
            std::move(detail), std::move(raw), 0};
}

RovResult<void> toVoidResult(
    RovResult<internal::ClientResponse> response)
{
    if (!response) {
        return RovResult<void>::fail(std::move(response.failure),
                                     response.sequence);
    }
    return RovResult<void>::success(response.sequence);
}

} // namespace

const char* toString(RovError error) noexcept
{
    switch (error) {
    case RovError::None: return "none";
    case RovError::BadArgument: return "bad_argument";
    case RovError::BadCommand: return "bad_command";
    case RovError::Busy: return "busy";
    case RovError::NotReady: return "not_ready";
    case RovError::Timeout: return "timeout";
    case RovError::Safety: return "safety";
    case RovError::Io: return "io";
    case RovError::Unsupported: return "unsupported";
    case RovError::ProtocolError: return "protocol_error";
    case RovError::Disconnected: return "disconnected";
    case RovError::TransportIo: return "transport_io";
    case RovError::SequenceExhausted: return "sequence_exhausted";
    }
    return "unknown";
}

const char* toString(ErrorOrigin origin) noexcept
{
    switch (origin) {
    case ErrorOrigin::None: return "none";
    case ErrorOrigin::M33: return "m33";
    case ErrorOrigin::Client: return "client";
    }
    return "unknown";
}

class RovControl::Impl final {
public:
    explicit Impl(std::unique_ptr<internal::ITransport> transport)
        : client(std::move(transport), std::chrono::milliseconds(1000))
    {
    }

    internal::RpmsgClient client;
};

RovControl::RovControl(std::string device)
    : impl_(std::make_unique<Impl>(
          std::make_unique<internal::PosixTtyTransport>(std::move(device))))
{
}

RovControl::RovControl(std::unique_ptr<internal::ITransport> transport)
    : impl_(std::make_unique<Impl>(std::move(transport)))
{
}

RovControl::~RovControl() = default;

RovResult<void> RovControl::open()
{
    return impl_->client.open();
}

void RovControl::close() noexcept
{
    impl_->client.close();
}

bool RovControl::isOpen() const noexcept
{
    return impl_->client.isOpen();
}

RovResult<void> RovControl::setServo(std::uint8_t id, std::uint8_t angle)
{
    if (id > 9U) {
        return RovResult<void>::fail(
            badArgument("servo id must be in range 0..9"));
    }
    if (angle > 180U) {
        return RovResult<void>::fail(
            badArgument("servo angle must be in range 0..180"));
    }

    const std::string payload = "set servo " + std::to_string(id) +
                                " " + std::to_string(angle);
    return toVoidResult(impl_->client.request(
        payload, internal::ExpectedResponseKind::Ack));
}

RovResult<void> RovControl::setAllServos(std::uint8_t angle)
{
    if (angle > 180U) {
        return RovResult<void>::fail(
            badArgument("servo angle must be in range 0..180"));
    }
    return toVoidResult(impl_->client.request(
        "set servo all " + std::to_string(angle),
        internal::ExpectedResponseKind::Ack));
}

RovResult<void> RovControl::centerServo(std::uint8_t id)
{
    if (id > 9U) {
        return RovResult<void>::fail(
            badArgument("servo id must be in range 0..9"));
    }
    return toVoidResult(impl_->client.request(
        "set servo " + std::to_string(id) + " mid",
        internal::ExpectedResponseKind::Ack));
}

RovResult<void> RovControl::centerAllServos()
{
    return toVoidResult(impl_->client.request(
        "set servo all mid", internal::ExpectedResponseKind::Ack));
}

RovResult<std::uint8_t> RovControl::getServo(std::uint8_t id)
{
    if (id > 9U) {
        return RovResult<std::uint8_t>::fail(
            badArgument("servo id must be in range 0..9"));
    }
    auto response = impl_->client.request(
        "get servo " + std::to_string(id),
        internal::ExpectedResponseKind::Servo);
    if (!response) {
        return RovResult<std::uint8_t>::fail(std::move(response.failure),
                                             response.sequence);
    }
    if (!response.value->servoId || !response.value->servoAngle ||
        *response.value->servoId != id) {
        return RovResult<std::uint8_t>::fail(
            badResponse("servo response id mismatch", response.value->raw),
            response.sequence);
    }
    return RovResult<std::uint8_t>::success(*response.value->servoAngle,
                                             response.sequence);
}

RovResult<std::array<std::uint8_t, 10>> RovControl::getAllServos()
{
    auto response = impl_->client.request(
        "get servo all", internal::ExpectedResponseKind::AllServos);
    if (!response) {
        return RovResult<std::array<std::uint8_t, 10>>::fail(
            std::move(response.failure), response.sequence);
    }
    return RovResult<std::array<std::uint8_t, 10>>::success(
        *response.value->servoAngles, response.sequence);
}

RovResult<void> RovControl::setVerticalBase(std::int16_t value)
{
    if (value < -100 || value > 100) {
        return RovResult<void>::fail(
            badArgument("propeller command must be in range -100..100"));
    }
    return toVoidResult(impl_->client.request(
        "set propeller vertical base " + std::to_string(value),
        internal::ExpectedResponseKind::Ack));
}

RovResult<void> RovControl::setVerticalPropeller(std::uint8_t id,
                                                 std::int16_t value)
{
    if (id < 10U || id > 13U) {
        return RovResult<void>::fail(
            badArgument("vertical propeller id must be in range 10..13"));
    }
    if (value < -100 || value > 100) {
        return RovResult<void>::fail(
            badArgument("propeller command must be in range -100..100"));
    }
    return toVoidResult(impl_->client.request(
        "set propeller vertical " + std::to_string(id) + " " +
            std::to_string(value), internal::ExpectedResponseKind::Ack));
}

RovResult<void> RovControl::setHorizontalBase(std::int16_t value)
{
    if (value < -100 || value > 100) {
        return RovResult<void>::fail(
            badArgument("propeller command must be in range -100..100"));
    }
    return toVoidResult(impl_->client.request(
        "set propeller horizontal base " + std::to_string(value),
        internal::ExpectedResponseKind::Ack));
}

RovResult<void> RovControl::setHorizontalPropeller(std::uint8_t id,
                                                   std::int16_t value)
{
    if (id < 14U || id > 15U) {
        return RovResult<void>::fail(
            badArgument("horizontal propeller id must be in range 14..15"));
    }
    if (value < -100 || value > 100) {
        return RovResult<void>::fail(
            badArgument("propeller command must be in range -100..100"));
    }
    return toVoidResult(impl_->client.request(
        "set propeller horizontal " + std::to_string(id) + " " +
            std::to_string(value), internal::ExpectedResponseKind::Ack));
}

namespace {

RovResult<std::int16_t> getPropeller(
    internal::RpmsgClient& client, std::uint8_t id,
    std::string_view field, internal::ExpectedResponseKind expected)
{
    if (id < 10U || id > 15U) {
        return RovResult<std::int16_t>::fail(
            badArgument("propeller id must be in range 10..15"));
    }
    auto response = client.request(
        "get propeller " + std::to_string(id) + " " + std::string(field),
        expected);
    if (!response) {
        return RovResult<std::int16_t>::fail(std::move(response.failure),
                                             response.sequence);
    }
    if (!response.value->propellerId || !response.value->propellerValue ||
        *response.value->propellerId != id) {
        return RovResult<std::int16_t>::fail(
            badResponse("propeller response id mismatch", response.value->raw),
            response.sequence);
    }
    return RovResult<std::int16_t>::success(*response.value->propellerValue,
                                             response.sequence);
}

RovResult<std::array<std::int16_t, 6>> getAllPropellers(
    internal::RpmsgClient& client, std::string_view field,
    internal::ExpectedResponseKind expected)
{
    auto response = client.request(
        "get propeller all " + std::string(field), expected);
    if (!response) {
        return RovResult<std::array<std::int16_t, 6>>::fail(
            std::move(response.failure), response.sequence);
    }
    return RovResult<std::array<std::int16_t, 6>>::success(
        *response.value->propellerValues, response.sequence);
}

} // namespace

RovResult<std::int16_t> RovControl::getPropellerBase(std::uint8_t id)
{
    return getPropeller(impl_->client, id, "base",
                        internal::ExpectedResponseKind::PropellerBase);
}

RovResult<std::int16_t> RovControl::getPropellerOutput(std::uint8_t id)
{
    return getPropeller(impl_->client, id, "real",
                        internal::ExpectedResponseKind::PropellerOutput);
}

RovResult<std::array<std::int16_t, 6>> RovControl::getAllPropellerBases()
{
    return getAllPropellers(
        impl_->client, "base",
        internal::ExpectedResponseKind::AllPropellerBases);
}

RovResult<std::array<std::int16_t, 6>> RovControl::getAllPropellerOutputs()
{
    return getAllPropellers(
        impl_->client, "real",
        internal::ExpectedResponseKind::AllPropellerOutputs);
}

RovResult<void> RovControl::enableStabilization()
{
    return toVoidResult(impl_->client.request(
        "horizontal on", internal::ExpectedResponseKind::Ack));
}

RovResult<void> RovControl::disableStabilization()
{
    return toVoidResult(impl_->client.request(
        "horizontal off", internal::ExpectedResponseKind::Ack));
}

RovResult<void> RovControl::enableHorizontalSynchronization()
{
    return toVoidResult(impl_->client.request(
        "synchronization on", internal::ExpectedResponseKind::Ack));
}

RovResult<void> RovControl::disableHorizontalSynchronization()
{
    return toVoidResult(impl_->client.request(
        "synchronization off", internal::ExpectedResponseKind::Ack));
}

RovResult<DypReading> RovControl::readDyp()
{
    auto response = impl_->client.request(
        "sensor dyp", internal::ExpectedResponseKind::Dyp);
    if (!response) {
        return RovResult<DypReading>::fail(std::move(response.failure),
                                           response.sequence);
    }
    if (!response.value || !response.value->dypReading) {
        return RovResult<DypReading>::fail(
            {RovError::ProtocolError, ErrorOrigin::Client,
             "DYP response did not contain a reading", {}, 0},
            response.sequence);
    }
    return RovResult<DypReading>::success(*response.value->dypReading,
                                           response.sequence);
}

RovResult<MpuRaw> RovControl::readMpu()
{
    auto response = impl_->client.request(
        "sensor mpu", internal::ExpectedResponseKind::Mpu);
    if (!response) {
        return RovResult<MpuRaw>::fail(std::move(response.failure),
                                       response.sequence);
    }
    return RovResult<MpuRaw>::success(*response.value->mpu,
                                      response.sequence);
}

RovResult<SensorSnapshot> RovControl::getSensorSnapshot()
{
    auto response = impl_->client.request(
        "sensor all", internal::ExpectedResponseKind::SensorSnapshot);
    if (!response) {
        return RovResult<SensorSnapshot>::fail(std::move(response.failure),
                                                response.sequence);
    }
    return RovResult<SensorSnapshot>::success(
        *response.value->sensorSnapshot, response.sequence);
}

RovResult<Attitude> RovControl::getAttitude()
{
    auto response = impl_->client.request(
        "get attitude", internal::ExpectedResponseKind::Attitude);
    if (!response) {
        return RovResult<Attitude>::fail(std::move(response.failure),
                                         response.sequence);
    }
    return RovResult<Attitude>::success(*response.value->attitude,
                                        response.sequence);
}

RovResult<StabilizationStatus> RovControl::getStabilization()
{
    auto response = impl_->client.request(
        "get stabilization", internal::ExpectedResponseKind::Stabilization);
    if (!response) {
        return RovResult<StabilizationStatus>::fail(
            std::move(response.failure), response.sequence);
    }
    return RovResult<StabilizationStatus>::success(
        *response.value->stabilization, response.sequence);
}

RovResult<void> RovControl::stop()
{
    return toVoidResult(impl_->client.request(
        "stop", internal::ExpectedResponseKind::Ack));
}

RovResult<void> RovControl::move()
{
    return toVoidResult(impl_->client.request(
        "move", internal::ExpectedResponseKind::Ack));
}

RovResult<void> RovControl::stopVertical()
{
    return toVoidResult(impl_->client.request(
        "stop vertical", internal::ExpectedResponseKind::Ack));
}

RovResult<void> RovControl::moveVertical()
{
    return toVoidResult(impl_->client.request(
        "move vertical", internal::ExpectedResponseKind::Ack));
}

RovResult<void> RovControl::stopHorizontal()
{
    return toVoidResult(impl_->client.request(
        "stop horizontal", internal::ExpectedResponseKind::Ack));
}

RovResult<void> RovControl::moveHorizontal()
{
    return toVoidResult(impl_->client.request(
        "move horizontal", internal::ExpectedResponseKind::Ack));
}

} // namespace rov
