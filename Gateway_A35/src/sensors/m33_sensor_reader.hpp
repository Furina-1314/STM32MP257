#ifndef GW_SENSORS_M33_SENSOR_READER_HPP
#define GW_SENSORS_M33_SENSOR_READER_HPP

#include "core/irov_control.hpp"
#include "sensors/sensor_types.hpp"

namespace gw::sensors {

// MPU6500 / DYP acquisition through RovControl. Shares the IRovControl
// instance with the command dispatcher: the underlying RpmsgClient
// serializes writes and dispatches responses by SEQ, so a blocking DYP
// wait cannot delay an Estop issued from the TCP thread (verified in the
// phase 0 source review; latency re-verified on the bench in phase 4).
class M33SensorReader
{
public:
    M33SensorReader(IRovControl& rov, const SensorConfig& config);

    // Reads raw MPU data and converts to SI units for the M33's current
    // +/-2g / +/-250dps configuration (constants in sensor_types.hpp; if
    // the M33 range configuration ever changes the constants must be
    // re-derived - phase 0 section 3.6).
    MpuSample pollMpu();

    // Triggers one DYP measurement. Distance is validated against the
    // configured physical window: UART-valid but out-of-window values
    // (e.g. 65533, not an acknowledged sentinel - D-10) and any device
    // failure (busy/timeout/io) yield an invalid sample carrying -1.0 mm.
    DypSample pollDyp();

private:
    IRovControl& rov_;
    SensorConfig config_; // by value: readers are often built from temporary configs
};

} // namespace gw::sensors

#endif // GW_SENSORS_M33_SENSOR_READER_HPP
