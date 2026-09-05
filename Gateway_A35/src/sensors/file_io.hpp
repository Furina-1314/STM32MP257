#ifndef GW_SENSORS_FILE_IO_HPP
#define GW_SENSORS_FILE_IO_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace gw::sensors {

// sysfs access helpers. All paths are plain open/read/close - no shell, no
// popen, no system(). The sysfs root is injectable so tests can point the
// readers at a virtual tree.

// Reads a small sysfs attribute fully; content is returned trimmed of
// surrounding whitespace. False on open/read failure.
bool readAttribute(const std::string& path, std::string& out);

// Strict signed 64-bit decimal parse of a whole trimmed string.
bool parseSigned64(const std::string& text, std::int64_t& out);

// Lists subdirectories of dir (non-recursive, names only).
bool listSubdirectories(const std::string& dir, std::vector<std::string>& out);

} // namespace gw::sensors

#endif // GW_SENSORS_FILE_IO_HPP
