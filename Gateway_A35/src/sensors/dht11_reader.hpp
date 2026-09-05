#ifndef GW_SENSORS_DHT11_READER_HPP
#define GW_SENSORS_DHT11_READER_HPP

#include <string>

#include "sensors/sensor_types.hpp"

namespace gw::sensors {

// DHT11 through the factory Alientek misc driver (default) or the standard
// Linux IIO driver. The misc ABI is /sys/class/misc/dht11/value with
// printable digits: "HH TT" concatenated - 4 digits = humidity*100+temp,
// 3 digits = leading zero lost (ambiguous), 2 digits = unusable (PDF 4.25
// and Drivers.md section 4). The IIO ABI exposes milli-units through
// in_temp_input / in_humidityrelative_input.
//
// Module loading (rmmod ds18b20 / modprobe dht11) is a deployment step and
// is deliberately NOT performed here.
class Dht11Reader
{
public:
    explicit Dht11Reader(const SensorConfig& config);

    // One synchronous acquisition attempt. Never throws, never blocks
    // longer than the sysfs read itself.
    Dht11Sample poll();

    // Individual backends (public for tests and diagnostics).
    Dht11Sample pollMisc();
    Dht11Sample pollIio();

private:
    Dht11Sample parseMiscValue(const std::string& raw) const;

    SensorConfig config_; // by value: readers are often built from temporary configs
    std::string cachedIioDir_; // remembered iio:deviceX for re-read; rescan
                               // happens when the read fails or on miss
};

} // namespace gw::sensors

#endif // GW_SENSORS_DHT11_READER_HPP
