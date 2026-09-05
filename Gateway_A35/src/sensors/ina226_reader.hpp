#ifndef GW_SENSORS_INA226_READER_HPP
#define GW_SENSORS_INA226_READER_HPP

#include <string>

#include "sensors/sensor_types.hpp"

namespace gw::sensors {

// INA226 through the kernel ina2xx hwmon driver. The device is discovered
// by scanning /sys/class/hwmon/hwmon*/name for "ina226" - never a hardcoded
// hwmon index. Attributes are read as signed 64-bit decimals and converted
// to engineering units (Drivers.md section 5):
//   in1_input     bus voltage, mV -> V (/1000)
//   in0_input     shunt voltage, mV (may be negative)
//   curr1_input   current, mA -> A (/1000, may be negative)
//   power1_input  power, uW -> W (/1e6)
//   shunt_resistor            uOhm
//   update_interval           ms
//
// If the node disappears (unplug, driver reload, hwmon renumbering) the
// next poll rescans.
class Ina226Reader
{
public:
    explicit Ina226Reader(const SensorConfig& config);

    Ina226Sample poll();

private:
    Ina226Sample readDevice(const std::string& hwmonDir);
    bool findDevice(std::string& outDir);

    SensorConfig config_; // by value: readers are often built from temporary configs
    std::string cachedDir_;
};

} // namespace gw::sensors

#endif // GW_SENSORS_INA226_READER_HPP
