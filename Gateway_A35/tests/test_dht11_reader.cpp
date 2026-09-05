// Dht11Reader: Alientek misc ABI (factory default), three-digit compat
// policy, ambiguity handling, range validation, standard IIO backend,
// auto fallback - all against a virtual sysfs tree.
#include "test_support.hpp"
#include "virtual_sysfs.hpp"

#include "sensors/dht11_reader.hpp"
#include "util/log.hpp"

#include <cmath>

namespace {

gw::sensors::SensorConfig configAt(const gw::VirtualSysfs& fs,
                          gw::sensors::Dht11Mode mode = gw::sensors::Dht11Mode::AlientekMisc,
                          bool compat = false)
{
    gw::sensors::SensorConfig config;
    config.sysfsRoot = fs.root();
    config.dht11Mode = mode;
    config.dht11ThreeDigitCompat = compat;
    return config;
}

void writeMisc(gw::VirtualSysfs& fs, const std::string& value)
{
    fs.writeFile("class/misc/dht11/value", value);
}

void writeIio(gw::VirtualSysfs& fs, const std::string& device,
              const std::string& tempMilli, const std::string& humidMilli)
{
    fs.writeFile("bus/iio/devices/" + device + "/name", "dht11\n");
    fs.writeFile("bus/iio/devices/" + device + "/in_temp_input", tempMilli);
    fs.writeFile("bus/iio/devices/" + device + "/in_humidityrelative_input",
                 humidMilli);
}

} // namespace

int main()
{
    using namespace gw;
    using namespace gw::sensors;
    using SC = sensors::SensorStatus;
    // Silence reader warnings during negative tests.
    setLogSink([](LogLevel, const char*) {});
    setLogLevel(LogLevel::Error);

    // Four-digit happy path, trailing newline trimmed.
    {
        VirtualSysfs fs;
        writeMisc(fs, "6929\n");
        sensors::Dht11Reader reader(configAt(fs));
        const sensors::Dht11Sample s = reader.poll();
        CHECK(s.ok());
        CHECK_EQ(static_cast<int>(s.humidityPercent), 69);
        CHECK_EQ(static_cast<int>(s.temperatureC), 29);
        CHECK(!s.inferred);
        CHECK_EQ(s.rawValue, "6929");
    }
    { // surrounding whitespace is trimmed
        VirtualSysfs fs;
        writeMisc(fs, "  9925  \n");
        sensors::Dht11Reader reader(configAt(fs));
        const sensors::Dht11Sample s = reader.poll();
        CHECK(s.ok());
        CHECK_EQ(static_cast<int>(s.humidityPercent), 99);
        CHECK_EQ(static_cast<int>(s.temperatureC), 25);
    }
    { // manual example: 99 %RH, 25 C
        VirtualSysfs fs;
        writeMisc(fs, "9925");
        sensors::Dht11Reader reader(configAt(fs));
        CHECK(reader.poll().ok());
    }

    // Out-of-range temperature is rejected (garbage guard).
    {
        VirtualSysfs fs;
        writeMisc(fs, "6999"); // 99 C
        sensors::Dht11Reader reader(configAt(fs));
        const sensors::Dht11Sample s = reader.poll();
        CHECK(s.status == SC::OutOfRange);
        CHECK(!s.ok());
    }

    // Three digits: ambiguous by default, inferable with the compat policy.
    {
        VirtualSysfs fs;
        writeMisc(fs, "692");
        sensors::Dht11Reader strict(configAt(fs));
        const sensors::Dht11Sample s = strict.poll();
        CHECK(s.status == SC::AmbiguousFormat);
        CHECK_EQ(s.rawValue, "692"); // raw preserved for diagnosis
        CHECK(!s.inferred);

        sensors::Dht11Reader compat(configAt(fs, Dht11Mode::AlientekMisc, true));
        const sensors::Dht11Sample c = compat.poll();
        CHECK(c.ok());
        CHECK_EQ(static_cast<int>(c.humidityPercent), 69);
        CHECK_EQ(static_cast<int>(c.temperatureC), 2);
        CHECK(c.inferred);
    }
    { // two digits stay ambiguous even with the policy
        VirtualSysfs fs;
        writeMisc(fs, "42");
        sensors::Dht11Reader compat(configAt(fs, Dht11Mode::AlientekMisc, true));
        CHECK(compat.poll().status == SC::AmbiguousFormat);
    }

    // Malformed content.
    {
        VirtualSysfs fs;
        writeMisc(fs, "");
        sensors::Dht11Reader reader(configAt(fs));
        CHECK(reader.poll().status == SC::ParseError);
    }
    {
        VirtualSysfs fs;
        writeMisc(fs, "12ab");
        sensors::Dht11Reader reader(configAt(fs));
        CHECK(reader.poll().status == SC::ParseError);
    }
    {
        VirtualSysfs fs;
        writeMisc(fs, "69290"); // five digits
        sensors::Dht11Reader reader(configAt(fs));
        CHECK(reader.poll().status == SC::AmbiguousFormat);
    }

    // Missing node: NotFound with the DS18B20 hint.
    {
        VirtualSysfs fs;
        sensors::Dht11Reader reader(configAt(fs));
        const sensors::Dht11Sample s = reader.poll();
        CHECK(s.status == SC::NotFound);
        CHECK(s.errorMessage.find("DS18B20") != std::string::npos);
    }

    // Standard IIO backend: milli-units, device discovered by name.
    {
        VirtualSysfs fs;
        fs.writeFile("bus/iio/devices/iio0/name", "something_else\n");
        writeIio(fs, "iio3", "29000", "69000");
        sensors::Dht11Reader reader(configAt(fs, Dht11Mode::StandardIio));
        const sensors::Dht11Sample s = reader.poll();
        CHECK(s.ok());
        CHECK(std::fabs(s.temperatureC - 29.0F) < 0.001F);
        CHECK(std::fabs(s.humidityPercent - 69.0F) < 0.001F);
    }
    { // iio out-of-range temperature
        VirtualSysfs fs;
        writeIio(fs, "iio1", "55000", "50000");
        sensors::Dht11Reader reader(configAt(fs, Dht11Mode::StandardIio));
        CHECK(reader.poll().status == SC::OutOfRange);
    }
    { // no dht11 among iio devices
        VirtualSysfs fs;
        fs.writeFile("bus/iio/devices/iio0/name", "bmp280\n");
        sensors::Dht11Reader reader(configAt(fs, Dht11Mode::StandardIio));
        CHECK(reader.poll().status == SC::NotFound);
    }

    // Auto mode: misc wins when present, IIO is the fallback.
    {
        VirtualSysfs fs;
        writeMisc(fs, "5020");
        writeIio(fs, "iio2", "25000", "55000");
        sensors::Dht11Reader reader(configAt(fs, Dht11Mode::Auto));
        const sensors::Dht11Sample s = reader.poll();
        CHECK(s.ok());
        CHECK_EQ(static_cast<int>(s.humidityPercent), 50); // misc value
        CHECK_EQ(static_cast<int>(s.temperatureC), 20);
    }
    {
        VirtualSysfs fs; // misc absent
        writeIio(fs, "iio2", "25000", "55000");
        sensors::Dht11Reader reader(configAt(fs, Dht11Mode::Auto));
        const sensors::Dht11Sample s = reader.poll();
        CHECK(s.ok());
        CHECK_EQ(static_cast<int>(s.humidityPercent), 55); // iio value
    }

    TEST_MAIN_END;
}
