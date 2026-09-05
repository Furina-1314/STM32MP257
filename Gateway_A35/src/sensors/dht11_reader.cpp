#include "sensors/dht11_reader.hpp"

#include <cstdlib>

#include "sensors/file_io.hpp"
#include "util/log.hpp"

namespace gw::sensors {

namespace {

bool allDigits(const std::string& text)
{
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if ((c < '0') || (c > '9')) {
            return false;
        }
    }
    return true;
}

} // namespace

Dht11Reader::Dht11Reader(const SensorConfig& config)
    : config_(config)
{
}

Dht11Sample Dht11Reader::poll()
{
    switch (config_.dht11Mode) {
    case Dht11Mode::AlientekMisc:
        return pollMisc();
    case Dht11Mode::StandardIio:
        return pollIio();
    case Dht11Mode::Auto:
    default:
        break;
    }
    // Auto: misc first; a missing misc node falls through to IIO.
    Dht11Sample sample = pollMisc();
    if (sample.status != SensorStatus::NotFound) {
        return sample;
    }
    return pollIio();
}

Dht11Sample Dht11Reader::pollMisc()
{
    Dht11Sample sample;
    sample.timestamp = SensorClock::now();

    const std::string path = config_.sysfsRoot + "/class/misc/dht11/value";
    std::string raw;
    if (!readAttribute(path, raw)) {
        sample.status = SensorStatus::NotFound;
        sample.errorMessage =
                "cannot read " + path
                + " (module loaded? DS18B20 may hold the shared GPIO)";
        return sample;
    }
    sample.rawValue = raw;
    return parseMiscValue(raw);
}

Dht11Sample Dht11Reader::parseMiscValue(const std::string& raw) const
{
    Dht11Sample sample;
    sample.timestamp = SensorClock::now();
    sample.rawValue = raw;

    if (raw.empty()) {
        sample.status = SensorStatus::ParseError;
        sample.errorMessage = "empty dht11 value";
        return sample;
    }
    if (!allDigits(raw)) {
        sample.status = SensorStatus::ParseError;
        sample.errorMessage = "non-numeric dht11 value: '" + raw + "'";
        return sample;
    }

    int humidity = 0;
    int temperature = 0;
    if (raw.size() == 4U) {
        humidity = std::atoi(raw.substr(0U, 2U).c_str());
        temperature = std::atoi(raw.substr(2U, 2U).c_str());
    } else if ((raw.size() == 3U) && config_.dht11ThreeDigitCompat) {
        // Manual-compatible inference: leading humidity zero was lost
        // ("6902" -> "6902"/"690 2"): first two digits humidity, the
        // remaining digit temperature. Never silent - flagged inferred.
        humidity = std::atoi(raw.substr(0U, 2U).c_str());
        temperature = std::atoi(raw.substr(2U, 1U).c_str());
        sample.inferred = true;
        logMessage(LogLevel::Warning,
                   "dht11: inferred three-digit value '" + raw + "'");
    } else {
        sample.status = SensorStatus::AmbiguousFormat;
        sample.errorMessage = "ambiguous dht11 value '" + raw
                + "' (2-3 digits without a unique split)";
        return sample;
    }

    if ((static_cast<float>(temperature) < config_.dht11TempMinC)
        || (static_cast<float>(temperature) > config_.dht11TempMaxC)
        || (static_cast<float>(humidity) < config_.dht11HumidityMinPct)
        || (static_cast<float>(humidity) > config_.dht11HumidityMaxPct)) {
        sample.status = SensorStatus::OutOfRange;
        sample.errorMessage = "dht11 value out of range: '" + raw + "'";
        return sample;
    }

    sample.humidityPercent = static_cast<float>(humidity);
    sample.temperatureC = static_cast<float>(temperature);
    sample.status = SensorStatus::Ok;
    return sample;
}

Dht11Sample Dht11Reader::pollIio()
{
    Dht11Sample sample;
    sample.timestamp = SensorClock::now();

    const std::string devicesDir = config_.sysfsRoot + "/bus/iio/devices";
    auto tryRead = [&](const std::string& deviceDir) -> bool {
        std::string name;
        if (!readAttribute(deviceDir + "/name", name) || (name != "dht11")) {
            return false; // not a dht11 device; keep scanning
        }
        std::string tempText;
        std::string humidText;
        std::int64_t tempMilli = 0;
        std::int64_t humidMilli = 0;
        if (!readAttribute(deviceDir + "/in_temp_input", tempText)
            || !parseSigned64(tempText, tempMilli)) {
            sample.status = SensorStatus::ParseError;
            sample.errorMessage = "bad in_temp_input in " + deviceDir;
            return true; // real result on a live dht11 node
        }
        if (!readAttribute(deviceDir + "/in_humidityrelative_input", humidText)
            || !parseSigned64(humidText, humidMilli)) {
            sample.status = SensorStatus::ParseError;
            sample.errorMessage =
                    "bad in_humidityrelative_input in " + deviceDir;
            return true;
        }
        const float temperature = static_cast<float>(tempMilli) / 1000.0F;
        const float humidity = static_cast<float>(humidMilli) / 1000.0F;
        sample.rawValue = tempText + "/" + humidText;
        if ((temperature < config_.dht11TempMinC)
            || (temperature > config_.dht11TempMaxC)
            || (humidity < config_.dht11HumidityMinPct)
            || (humidity > config_.dht11HumidityMaxPct)) {
            sample.status = SensorStatus::OutOfRange;
            sample.errorMessage = "dht11 iio value out of range";
            return true;
        }
        sample.temperatureC = temperature;
        sample.humidityPercent = humidity;
        sample.status = SensorStatus::Ok;
        return true;
    };

    // Reuse the remembered device while it still carries the dht11 name;
    // otherwise drop the cache and rescan (device may have re-enumerated).
    if (!cachedIioDir_.empty()) {
        const std::string full = devicesDir + "/" + cachedIioDir_;
        std::string name;
        if (readAttribute(full + "/name", name) && (name == "dht11")) {
            if (tryRead(full)) {
                return sample;
            }
        }
        cachedIioDir_.clear();
    }

    std::vector<std::string> devices;
    if (!listSubdirectories(devicesDir, devices)) {
        sample.status = SensorStatus::NotFound;
        sample.errorMessage = "cannot list " + devicesDir;
        return sample;
    }
    for (const std::string& device : devices) {
        // No filename filter: on Linux the directory set is exactly the
        // kernel's iio:deviceX entries and the name attribute is the
        // authoritative matcher (also keeps virtual-sysfs tests portable:
        // Windows forbids ':' in directory names).
        if (tryRead(devicesDir + "/" + device)) {
            if (sample.status == SensorStatus::Ok) {
                cachedIioDir_ = device;
            }
            return sample;
        }
    }
    sample.status = SensorStatus::NotFound;
    sample.errorMessage = "no dht11 iio device under " + devicesDir;
    return sample;
}

} // namespace gw::sensors
