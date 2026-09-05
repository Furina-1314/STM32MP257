#ifndef ROV_INTERNAL_RESPONSE_PARSER_HPP
#define ROV_INTERNAL_RESPONSE_PARSER_HPP

#include "rov/rov_result.hpp"
#include "rov/rov_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rov::internal {

enum class ParsedLineKind {
    Response,
    Event,
    Unsolicited,
    Malformed
};

enum class ResponseKind {
    Success,
    Error
};

enum class ExpectedResponseKind {
    Ack,
    Dyp,
    Servo,
    AllServos,
    PropellerBase,
    PropellerOutput,
    AllPropellerBases,
    AllPropellerOutputs,
    Mpu,
    SensorSnapshot,
    Attitude,
    Stabilization
};

struct ParsedLine {
    ParsedLineKind lineKind{ParsedLineKind::Malformed};
    ResponseKind responseKind{ResponseKind::Success};
    bool hasSequence{false};
    std::uint16_t sequence{0};
    RovError error{RovError::None};
    std::string successPayload;
    std::string raw;
    std::string detail;
};

struct ClientResponse {
    std::uint16_t sequence{0};
    std::string raw;
    std::optional<DypReading> dypReading;
    std::optional<std::uint8_t> servoId;
    std::optional<std::uint8_t> servoAngle;
    std::optional<std::array<std::uint8_t, 10>> servoAngles;
    std::optional<std::uint8_t> propellerId;
    std::optional<std::int16_t> propellerValue;
    std::optional<std::array<std::int16_t, 6>> propellerValues;
    std::optional<MpuRaw> mpu;
    std::optional<SensorSnapshot> sensorSnapshot;
    std::optional<Attitude> attitude;
    std::optional<StabilizationStatus> stabilization;
};

class ResponseParser final {
public:
    static ParsedLine parseLine(std::string_view line);
    static RovResult<ClientResponse> validate(
        const ParsedLine& line,
        ExpectedResponseKind expected);
};

} // namespace rov::internal

#endif // ROV_INTERNAL_RESPONSE_PARSER_HPP
