#include "sensors/ina226_reader.hpp"

#include "sensors/file_io.hpp"
#include "util/log.hpp"

namespace gw::sensors {

namespace {

bool readInt64(const std::string& path, std::int64_t& out)
{
    std::string text;
    if (!readAttribute(path, text)) {
        return false;
    }
    return parseSigned64(text, out);
}

} // namespace

Ina226Reader::Ina226Reader(const SensorConfig& config)
    : config_(config)
{
}

bool Ina226Reader::findDevice(std::string& outDir)
{
    const std::string classDir = config_.sysfsRoot + "/class/hwmon";
    std::vector<std::string> entries;
    if (!listSubdirectories(classDir, entries)) {
        return false;
    }
    for (const std::string& entry : entries) {
        if (entry.rfind("hwmon", 0) != 0) {
            continue;
        }
        std::string name;
        if (readAttribute(classDir + "/" + entry + "/name", name)
            && (name == "ina226")) {
            outDir = classDir + "/" + entry;
            return true;
        }
    }
    return false;
}

Ina226Sample Ina226Reader::readDevice(const std::string& hwmonDir)
{
    Ina226Sample sample;
    sample.timestamp = SensorClock::now();

    std::int64_t busMv = 0;
    if (!readInt64(hwmonDir + "/in1_input", busMv)) {
        sample.status = SensorStatus::ReadError;
        sample.errorMessage = "missing in1_input in " + hwmonDir;
        return sample;
    }

    std::int64_t value = 0;
    if (readInt64(hwmonDir + "/in0_input", value)) {
        sample.shuntVoltageMv = static_cast<float>(value);
        sample.hasShuntVoltage = true;
    }
    if (readInt64(hwmonDir + "/curr1_input", value)) {
        sample.currentA = static_cast<float>(value) / 1000.0F;
        sample.hasCurrent = true;
    }
    if (readInt64(hwmonDir + "/power1_input", value)) {
        sample.powerW = static_cast<float>(value) / 1000000.0F;
        sample.hasPower = true;
    }
    if (readInt64(hwmonDir + "/shunt_resistor", value)) {
        sample.shuntResistorUohm = value;
        sample.hasShuntResistor = true;
    }
    // update_interval is informational; ignore absence.

    sample.busVoltageV = static_cast<float>(busMv) / 1000.0F;
    if ((sample.busVoltageV < config_.busVoltageMinV)
        || (sample.busVoltageV > config_.busVoltageMaxV)) {
        sample.status = SensorStatus::OutOfRange;
        sample.errorMessage = "bus voltage out of range: "
                + std::to_string(sample.busVoltageV) + " V";
        return sample;
    }
    sample.status = SensorStatus::Ok;
    return sample;
}

Ina226Sample Ina226Reader::poll()
{
    // Fast path: the remembered node still answers.
    if (!cachedDir_.empty()) {
        std::string name;
        if (readAttribute(cachedDir_ + "/name", name) && (name == "ina226")) {
            return readDevice(cachedDir_);
        }
        cachedDir_.clear(); // renumbered or gone: rescan
    }

    Ina226Sample sample;
    sample.timestamp = SensorClock::now();
    std::string dir;
    if (!findDevice(dir)) {
        sample.status = SensorStatus::NotFound;
        sample.errorMessage = "no ina226 hwmon node under "
                + config_.sysfsRoot + "/class/hwmon"
                + " (module loaded? DT ti,ina226 node present?)";
        return sample;
    }
    cachedDir_ = dir;
    return readDevice(dir);
}

} // namespace gw::sensors
