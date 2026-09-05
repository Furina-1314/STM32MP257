#ifndef GW_UTIL_INI_CONFIG_HPP
#define GW_UTIL_INI_CONFIG_HPP

#include <cstdint>
#include <map>
#include <string>

namespace gw {

// Minimal INI reader for the gateway configuration: [section] headers,
// key=value pairs, '#' or ';' comments, surrounding whitespace trimmed.
// Missing files/keys fall back to caller-provided defaults so the gateway
// also runs with no configuration file at all.
class IniConfig
{
public:
    // Returns false when the file exists but cannot be read. An empty or
    // comment-only file is valid.
    bool load(const std::string& path);

    bool has(const std::string& section, const std::string& key) const;

    std::string get(const std::string& section, const std::string& key,
                    const std::string& fallback) const;
    int getInt(const std::string& section, const std::string& key,
               int fallback) const;
    bool getBool(const std::string& section, const std::string& key,
                 bool fallback) const;

    // Number of stored keys (diagnostics/self-check).
    std::size_t size() const { return values_.size(); }

private:
    // "section/key" -> value
    std::map<std::string, std::string> values_;
};

} // namespace gw

#endif // GW_UTIL_INI_CONFIG_HPP
