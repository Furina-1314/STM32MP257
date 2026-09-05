// Ina226Reader: hwmon discovery by name (no hardcoded index), unit
// conversions including negative current, optional attributes, range
// checks, disappearance and renumbering with rescan.
#include "test_support.hpp"
#include "virtual_sysfs.hpp"

#include "sensors/ina226_reader.hpp"
#include "util/log.hpp"

#include <cmath>

namespace {

gw::sensors::SensorConfig configAt(const gw::VirtualSysfs& fs)
{
    gw::sensors::SensorConfig config;
    config.sysfsRoot = fs.root();
    return config;
}

void writeIna(gw::VirtualSysfs& fs, const std::string& hwmon,
              const std::string& in1 = "4955",
              const std::string* in0 = nullptr,
              const std::string* curr = nullptr,
              const std::string* power = nullptr,
              const std::string* shunt = nullptr)
{
    fs.writeFile("class/hwmon/" + hwmon + "/name", "ina226\n");
    fs.writeFile("class/hwmon/" + hwmon + "/in1_input", in1);
    if (in0 != nullptr) {
        fs.writeFile("class/hwmon/" + hwmon + "/in0_input", *in0);
    }
    if (curr != nullptr) {
        fs.writeFile("class/hwmon/" + hwmon + "/curr1_input", *curr);
    }
    if (power != nullptr) {
        fs.writeFile("class/hwmon/" + hwmon + "/power1_input", *power);
    }
    if (shunt != nullptr) {
        fs.writeFile("class/hwmon/" + hwmon + "/shunt_resistor", *shunt);
    }
    fs.writeFile("class/hwmon/" + hwmon + "/update_interval", "1100");
}

bool closeTo(float actual, float expected)
{
    return std::fabs(actual - expected) < 0.0005F;
}

} // namespace

int main()
{
    using namespace gw;
    using SC = sensors::SensorStatus;
    setLogSink([](LogLevel, const char*) {});
    setLogLevel(LogLevel::Error);

    // Discovery skips foreign hwmon nodes and picks the ina226 one.
    {
        VirtualSysfs fs;
        fs.writeFile("class/hwmon/hwmon0/name", "coretemp\n");
        fs.writeFile("class/hwmon/hwmon0/temp1_input", "45000");
        const std::string in0 = "-12";
        const std::string curr = "-1500";
        const std::string power = "7500000";
        const std::string shunt = "10000";
        writeIna(fs, "hwmon1", "4955", &in0, &curr, &power, &shunt);
        fs.writeFile("class/hwmon/hwmon2/name", "fancontrol\n");

        sensors::Ina226Reader reader(configAt(fs));
        const sensors::Ina226Sample s = reader.poll();
        CHECK(s.ok());
        CHECK(closeTo(s.busVoltageV, 4.955F));
        CHECK(closeTo(s.shuntVoltageMv, -12.0F));
        CHECK(closeTo(s.currentA, -1.5F)); // negative current parses
        CHECK(closeTo(s.powerW, 7.5F));
        CHECK_EQ(s.shuntResistorUohm, static_cast<std::int64_t>(10000));
        CHECK(s.hasShuntVoltage && s.hasCurrent && s.hasPower
              && s.hasShuntResistor);
    }

    // Optional attributes missing: still Ok with flags clear.
    {
        VirtualSysfs fs;
        writeIna(fs, "hwmon3", "16000"); // only in1_input
        sensors::Ina226Reader reader(configAt(fs));
        const sensors::Ina226Sample s = reader.poll();
        CHECK(s.ok());
        CHECK(closeTo(s.busVoltageV, 16.0F));
        CHECK(!s.hasShuntVoltage && !s.hasCurrent && !s.hasPower
              && !s.hasShuntResistor);
    }

    // Required attribute missing: read error, not a crash.
    {
        VirtualSysfs fs;
        fs.writeFile("class/hwmon/hwmon1/name", "ina226\n");
        sensors::Ina226Reader reader(configAt(fs));
        const sensors::Ina226Sample s = reader.poll();
        CHECK(s.status == SC::ReadError);
    }

    // Garbage attribute content: parse error surfaces as read failure.
    {
        VirtualSysfs fs;
        fs.writeFile("class/hwmon/hwmon1/name", "ina226\n");
        fs.writeFile("class/hwmon/hwmon1/in1_input", "not-a-number");
        sensors::Ina226Reader reader(configAt(fs));
        CHECK(reader.poll().status == SC::ReadError);
    }

    // Out-of-range bus voltage.
    {
        VirtualSysfs fs;
        writeIna(fs, "hwmon1", "40000"); // 40 V > 36 V cap
        sensors::Ina226Reader reader(configAt(fs));
        const sensors::Ina226Sample s = reader.poll();
        CHECK(s.status == SC::OutOfRange);
    }

    // No ina226 anywhere: NotFound with diagnostics.
    {
        VirtualSysfs fs;
        fs.writeFile("class/hwmon/hwmon0/name", "coretemp\n");
        sensors::Ina226Reader reader(configAt(fs));
        const sensors::Ina226Sample s = reader.poll();
        CHECK(s.status == SC::NotFound);
        CHECK(!s.errorMessage.empty());
    }

    // Renumbering: cached node disappears, rescan finds it under a new
    // index without service restart.
    {
        VirtualSysfs fs;
        writeIna(fs, "hwmon1", "12000");
        sensors::Ina226Reader reader(configAt(fs));
        CHECK(reader.poll().ok());

        fs.removeFile("class/hwmon/hwmon1/in1_input");
        fs.removeFile("class/hwmon/hwmon1/name");
        fs.removeDir("class/hwmon/hwmon1");
        sensors::Ina226Sample s = reader.poll(); // cached node gone
        CHECK(!s.ok());

        writeIna(fs, "hwmon7", "12000"); // re-appeared elsewhere
        s = reader.poll();
        CHECK(s.ok());
        CHECK(closeTo(s.busVoltageV, 12.0F));
    }

    TEST_MAIN_END;
}
