#include "internal/response_parser.hpp"

#include <charconv>
#include <cctype>
#include <chrono>
#include <limits>
#include <string_view>
#include <vector>

namespace rov::internal {
namespace {

bool startsWith(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

RovError parseWireError(std::string_view value)
{
    if (value == "bad_arg") {
        return RovError::BadArgument;
    }
    if (value == "bad_cmd") {
        return RovError::BadCommand;
    }
    if (value == "busy") {
        return RovError::Busy;
    }
    if (value == "not_ready") {
        return RovError::NotReady;
    }
    if (value == "timeout") {
        return RovError::Timeout;
    }
    if (value == "safety") {
        return RovError::Safety;
    }
    if (value == "io") {
        return RovError::Io;
    }
    if (value == "unsupported") {
        return RovError::Unsupported;
    }
    return RovError::ProtocolError;
}

RovFailure protocolFailure(const ParsedLine& line, std::string detail)
{
    return {RovError::ProtocolError, ErrorOrigin::Client,
            std::move(detail), line.raw, 0};
}

std::vector<std::string_view> tokenize(std::string_view payload)
{
    std::vector<std::string_view> tokens;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        while (offset < payload.size() &&
               std::isspace(static_cast<unsigned char>(payload[offset])) != 0) {
            ++offset;
        }
        if (offset == payload.size()) {
            break;
        }
        const std::size_t end = payload.find_first_of(" \t", offset);
        tokens.push_back(payload.substr(offset, end - offset));
        if (end == std::string_view::npos) {
            break;
        }
        offset = end;
    }
    return tokens;
}

template <typename T>
bool parseSigned(std::string_view text, T& value)
{
    std::int64_t parsed = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    if (text.empty() || result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() ||
        parsed < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
        parsed > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
        return false;
    }
    value = static_cast<T>(parsed);
    return true;
}

template <typename T>
bool parseUnsigned(std::string_view text, T& value)
{
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    if (text.empty() || result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() ||
        parsed > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
        return false;
    }
    value = static_cast<T>(parsed);
    return true;
}

bool parseBool(std::string_view text, bool& value)
{
    if (text == "0") {
        value = false;
        return true;
    }
    if (text == "1") {
        value = true;
        return true;
    }
    return false;
}

bool parseReady(std::string_view text, bool& value)
{
    if (text == "ready") {
        value = true;
        return true;
    }
    if (text == "not_ready") {
        value = false;
        return true;
    }
    return false;
}

bool parseDypState(std::string_view text, DypState& value)
{
    if (text == "uninitialized") value = DypState::Uninitialized;
    else if (text == "idle") value = DypState::Idle;
    else if (text == "waiting") value = DypState::Waiting;
    else if (text == "complete") value = DypState::Complete;
    else if (text == "timeout") value = DypState::Timeout;
    else if (text == "io_error") value = DypState::IoError;
    else return false;
    return true;
}

RovResult<ClientResponse> malformed(const ParsedLine& line,
                                    std::string detail)
{
    return RovResult<ClientResponse>::fail(
        protocolFailure(line, std::move(detail)), line.sequence);
}

RovResult<ClientResponse> success(const ParsedLine& line,
                                  ClientResponse response)
{
    return RovResult<ClientResponse>::success(std::move(response),
                                               line.sequence);
}

} // namespace

ParsedLine ResponseParser::parseLine(std::string_view line)
{
    ParsedLine parsed;
    parsed.raw.assign(line.begin(), line.end());

    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
        parsed.raw.assign(line.begin(), line.end());
    }
    if (startsWith(line, "event ") || line == "event") {
        parsed.lineKind = ParsedLineKind::Event;
        return parsed;
    }

    const std::size_t separator = line.find_first_of(" \t");
    if (separator == std::string_view::npos) {
        parsed.lineKind = ParsedLineKind::Unsolicited;
        return parsed;
    }

    const std::string_view sequenceText = line.substr(0, separator);
    std::size_t payloadStart = separator;
    while (payloadStart < line.size() &&
           std::isspace(static_cast<unsigned char>(line[payloadStart])) != 0) {
        ++payloadStart;
    }
    const std::string_view payload = line.substr(payloadStart);

    bool sequenceValid = sequenceText.size() == 4;
    std::uint16_t sequence = 0;
    if (sequenceValid) {
        for (char character : sequenceText) {
            if (std::isdigit(static_cast<unsigned char>(character)) == 0) {
                sequenceValid = false;
                break;
            }
            sequence = static_cast<std::uint16_t>(
                sequence * 10U + static_cast<unsigned int>(character - '0'));
        }
    }

    if (!sequenceValid) {
        if (startsWith(payload, "ok") || startsWith(payload, "err")) {
            parsed.lineKind = ParsedLineKind::Malformed;
            parsed.detail = "invalid four-digit response sequence";
        } else {
            parsed.lineKind = ParsedLineKind::Unsolicited;
        }
        return parsed;
    }

    parsed.hasSequence = true;
    parsed.sequence = sequence;
    if (payload == "ok") {
        parsed.lineKind = ParsedLineKind::Response;
        parsed.responseKind = ResponseKind::Success;
        return parsed;
    }
    if (startsWith(payload, "ok ")) {
        parsed.lineKind = ParsedLineKind::Response;
        parsed.responseKind = ResponseKind::Success;
        parsed.successPayload.assign(payload.substr(3));
        return parsed;
    }
    if (startsWith(payload, "err ")) {
        const std::string_view errorText = payload.substr(4);
        const RovError error = parseWireError(errorText);
        if (error == RovError::ProtocolError) {
            parsed.lineKind = ParsedLineKind::Malformed;
            parsed.detail = "unknown or malformed M33 error response";
            return parsed;
        }
        parsed.lineKind = ParsedLineKind::Response;
        parsed.responseKind = ResponseKind::Error;
        parsed.error = error;
        return parsed;
    }

    parsed.lineKind = ParsedLineKind::Malformed;
    parsed.detail = "sequenced line is neither ok nor err";
    return parsed;
}

RovResult<ClientResponse> ResponseParser::validate(
    const ParsedLine& line,
    ExpectedResponseKind expected)
{
    if (line.lineKind != ParsedLineKind::Response) {
        return RovResult<ClientResponse>::fail(
            protocolFailure(line, line.detail.empty()
                ? "line is not a sequenced response" : line.detail));
    }

    if (line.responseKind == ResponseKind::Error) {
        return RovResult<ClientResponse>::fail(
            {line.error, ErrorOrigin::M33, "M33 returned an error",
             line.raw, 0}, line.sequence);
    }

    ClientResponse response;
    response.sequence = line.sequence;
    response.raw = line.raw;

    if (expected == ExpectedResponseKind::Ack) {
        if (!line.successPayload.empty()) {
            return RovResult<ClientResponse>::fail(
                protocolFailure(line, "unexpected payload in ok response"),
                line.sequence);
        }
        return RovResult<ClientResponse>::success(std::move(response),
                                                   line.sequence);
    }

    const auto tokens = tokenize(line.successPayload);

    if (expected == ExpectedResponseKind::Dyp) {
        std::uint16_t distance = 0;
        if (tokens.size() != 3U || tokens[0] != "dyp" ||
            tokens[1] != "distance_mm" ||
            !parseUnsigned(tokens[2], distance)) {
            return malformed(line, "malformed DYP success payload");
        }
        response.dypReading = DypReading{distance};
        return success(line, std::move(response));
    }

    if (expected == ExpectedResponseKind::Servo) {
        std::uint8_t id = 0;
        std::uint8_t angle = 0;
        if (tokens.size() != 3U || tokens[0] != "servo" ||
            !parseUnsigned(tokens[1], id) || id > 9U ||
            !parseUnsigned(tokens[2], angle) || angle > 180U) {
            return malformed(line, "malformed servo success payload");
        }
        response.servoId = id;
        response.servoAngle = angle;
        return success(line, std::move(response));
    }

    if (expected == ExpectedResponseKind::AllServos) {
        std::array<std::uint8_t, 10> angles{};
        if (tokens.size() != 12U || tokens[0] != "servo" ||
            tokens[1] != "all") {
            return malformed(line, "malformed all-servo success payload");
        }
        for (std::size_t index = 0; index < angles.size(); ++index) {
            if (!parseUnsigned(tokens[index + 2U], angles[index]) ||
                angles[index] > 180U) {
                return malformed(line, "invalid all-servo angle");
            }
        }
        response.servoAngles = angles;
        return success(line, std::move(response));
    }

    const bool propellerSingle =
        expected == ExpectedResponseKind::PropellerBase ||
        expected == ExpectedResponseKind::PropellerOutput;
    const bool propellerAll =
        expected == ExpectedResponseKind::AllPropellerBases ||
        expected == ExpectedResponseKind::AllPropellerOutputs;
    if (propellerSingle) {
        const std::string_view field =
            expected == ExpectedResponseKind::PropellerBase ? "base" : "real";
        std::uint8_t id = 0;
        std::int16_t value = 0;
        if (tokens.size() != 4U || tokens[0] != "propeller" ||
            !parseUnsigned(tokens[1], id) || id < 10U || id > 15U ||
            tokens[2] != field || !parseSigned(tokens[3], value) ||
            value < -100 || value > 100) {
            return malformed(line, "malformed propeller success payload");
        }
        response.propellerId = id;
        response.propellerValue = value;
        return success(line, std::move(response));
    }
    if (propellerAll) {
        const std::string_view field =
            expected == ExpectedResponseKind::AllPropellerBases
                ? "base" : "real";
        std::array<std::int16_t, 6> values{};
        if (tokens.size() != 9U || tokens[0] != "propeller" ||
            tokens[1] != "all" || tokens[2] != field) {
            return malformed(line, "malformed all-propeller payload");
        }
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (!parseSigned(tokens[index + 3U], values[index]) ||
                values[index] < -100 || values[index] > 100) {
                return malformed(line, "invalid all-propeller value");
            }
        }
        response.propellerValues = values;
        return success(line, std::move(response));
    }

    if (expected == ExpectedResponseKind::Mpu) {
        MpuRaw mpu;
        std::array<std::int16_t*, 6> fields{{
            &mpu.ax, &mpu.ay, &mpu.az, &mpu.gx, &mpu.gy, &mpu.gz}};
        if (tokens.size() != fields.size()) {
            return malformed(line, "malformed MPU success payload");
        }
        for (std::size_t index = 0; index < fields.size(); ++index) {
            if (!parseSigned(tokens[index], *fields[index])) {
                return malformed(line, "invalid MPU int16 value");
            }
        }
        response.mpu = mpu;
        return success(line, std::move(response));
    }

    if (expected == ExpectedResponseKind::SensorSnapshot) {
        SensorSnapshot snapshot;
        bool valid = false;
        if (tokens.size() != 15U || tokens[0] != "sensors" ||
            tokens[1] != "mpu" || !parseReady(tokens[2], snapshot.mpuReady) ||
            tokens[3] != "dyp" || !parseReady(tokens[4], snapshot.dypReady) ||
            tokens[5] != "state" ||
            !parseDypState(tokens[6], snapshot.dypState) ||
            tokens[7] != "busy" || !parseBool(tokens[8], snapshot.dypBusy) ||
            tokens[9] != "valid" || !parseBool(tokens[10], valid) ||
            tokens[11] != "distance_mm" || tokens[13] != "age_ms") {
            return malformed(line, "malformed sensor snapshot payload");
        }
        if (!valid) {
            if (tokens[12] != "invalid" || tokens[14] != "invalid") {
                return malformed(line, "invalid sensor cache markers");
            }
        } else {
            std::uint16_t distance = 0;
            using MillisecondsRep = std::chrono::milliseconds::rep;
            MillisecondsRep age = 0;
            if (!parseUnsigned(tokens[12], distance) ||
                !parseSigned(tokens[14], age) || age < 0) {
                return malformed(line, "invalid sensor cache values");
            }
            snapshot.distanceMm = distance;
            snapshot.age = std::chrono::milliseconds(age);
        }
        response.sensorSnapshot = snapshot;
        return success(line, std::move(response));
    }

    if (expected == ExpectedResponseKind::Attitude) {
        std::int32_t roll = 0;
        std::int32_t pitch = 0;
        Attitude attitude;
        if (tokens.size() != 7U || tokens[0] != "attitude" ||
            tokens[1] != "roll" || !parseSigned(tokens[2], roll) ||
            tokens[3] != "pitch" || !parseSigned(tokens[4], pitch) ||
            tokens[5] != "ready" || !parseBool(tokens[6], attitude.ready)) {
            return malformed(line, "malformed attitude payload");
        }
        attitude.rollDegrees = static_cast<double>(roll) / 100.0;
        attitude.pitchDegrees = static_cast<double>(pitch) / 100.0;
        response.attitude = attitude;
        return success(line, std::move(response));
    }

    if (expected == ExpectedResponseKind::Stabilization) {
        StabilizationStatus status;
        std::array<std::int32_t, 4> scaled{};
        if (tokens.size() != 26U || tokens[0] != "stabilization" ||
            tokens[1] != "re" || !parseSigned(tokens[2], scaled[0]) ||
            tokens[3] != "pe" || !parseSigned(tokens[4], scaled[1]) ||
            tokens[5] != "rp" || !parseSigned(tokens[6], scaled[2]) ||
            tokens[7] != "pp" || !parseSigned(tokens[8], scaled[3]) ||
            tokens[9] != "ch") {
            return malformed(line, "malformed stabilization payload");
        }
        for (std::size_t index = 0;
             index < status.verticalCorrections.size(); ++index) {
            if (!parseSigned(tokens[index + 10U],
                             status.verticalCorrections[index])) {
                return malformed(line, "invalid stabilization correction");
            }
        }
        if (tokens[14] != "ar" || !parseBool(tokens[15], status.attitudeReady) ||
            tokens[16] != "af" || !parseBool(tokens[17], status.attitudeFresh) ||
            tokens[18] != "he" || !parseBool(tokens[19], status.horizontalEnabled) ||
            tokens[20] != "gs" || !parseBool(tokens[21], status.globalStopped) ||
            tokens[22] != "vs" || !parseBool(tokens[23], status.verticalStopped) ||
            tokens[24] != "hs" || !parseBool(tokens[25], status.horizontalStopped)) {
            return malformed(line, "invalid stabilization flags");
        }
        status.rollErrorDegrees = static_cast<double>(scaled[0]) / 100.0;
        status.pitchErrorDegrees = static_cast<double>(scaled[1]) / 100.0;
        status.rollPidCommand = static_cast<double>(scaled[2]) / 100.0;
        status.pitchPidCommand = static_cast<double>(scaled[3]) / 100.0;
        response.stabilization = status;
        return success(line, std::move(response));
    }

    return malformed(line, "unsupported expected response kind");
}

} // namespace rov::internal
