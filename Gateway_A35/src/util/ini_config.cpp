#include "util/ini_config.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace gw {

namespace {

std::string trimmed(const std::string& text)
{
    std::size_t begin = 0U;
    std::size_t end = text.size();
    while ((begin < end) && ((text[begin] == ' ') || (text[begin] == '\t')
                             || (text[begin] == '\r') || (text[begin] == '\n'))) {
        ++begin;
    }
    while ((end > begin)
           && ((text[end - 1U] == ' ') || (text[end - 1U] == '\t')
               || (text[end - 1U] == '\r') || (text[end - 1U] == '\n'))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

} // namespace

bool IniConfig::load(const std::string& path)
{
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    std::string section;
    std::string line;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
        line = trimmed(buffer);
        if (line.empty() || (line[0] == '#') || (line[0] == ';')) {
            continue;
        }
        if ((line.front() == '[') && (line.back() == ']')
            && (line.size() >= 3U)) {
            section = trimmed(line.substr(1U, line.size() - 2U));
            continue;
        }
        const std::string::size_type eq = line.find('=');
        if (eq == std::string::npos) {
            continue; // malformed line: ignored, defaults win
        }
        const std::string key = trimmed(line.substr(0U, eq));
        const std::string value = trimmed(line.substr(eq + 1U));
        if (key.empty() || section.empty()) {
            continue;
        }
        values_[section + "/" + key] = value;
    }
    std::fclose(file);
    return true;
}

bool IniConfig::has(const std::string& section, const std::string& key) const
{
    return values_.find(section + "/" + key) != values_.end();
}

std::string IniConfig::get(const std::string& section, const std::string& key,
                           const std::string& fallback) const
{
    const auto it = values_.find(section + "/" + key);
    return (it == values_.end()) ? fallback : it->second;
}

int IniConfig::getInt(const std::string& section, const std::string& key,
                      int fallback) const
{
    const auto it = values_.find(section + "/" + key);
    if (it == values_.end()) {
        return fallback;
    }
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(it->second.c_str(), &end, 10);
    if ((errno != 0) || (end == nullptr) || (*end != '\0') || (end == it->second.c_str())) {
        return fallback;
    }
    return static_cast<int>(value);
}

bool IniConfig::getBool(const std::string& section, const std::string& key,
                        bool fallback) const
{
    const auto it = values_.find(section + "/" + key);
    if (it == values_.end()) {
        return fallback;
    }
    const std::string& v = it->second;
    if ((v == "1") || (v == "true") || (v == "yes") || (v == "on")) {
        return true;
    }
    if ((v == "0") || (v == "false") || (v == "no") || (v == "off")) {
        return false;
    }
    return fallback;
}

} // namespace gw
