#include "sensors/file_io.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>

namespace gw::sensors {

namespace {

std::string trimmed(const std::string& text)
{
    std::size_t begin = 0U;
    std::size_t end = text.size();
    while ((begin < end)
           && ((text[begin] == ' ') || (text[begin] == '\t')
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

bool readAttribute(const std::string& path, std::string& out)
{
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    std::string content;
    char buffer[256];
    std::size_t got = 0U;
    while ((got = std::fread(buffer, 1U, sizeof(buffer), file)) > 0U) {
        content.append(buffer, got);
        if (content.size() > 1024U) { // sysfs attributes are tiny
            break;
        }
    }
    const bool failed = (std::ferror(file) != 0);
    std::fclose(file);
    if (failed) {
        return false;
    }
    out = trimmed(content);
    return true;
}

bool parseSigned64(const std::string& text, std::int64_t& out)
{
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if ((errno != 0) || (end == nullptr) || (*end != '\0')) {
        return false;
    }
    if ((end == text.c_str()) && (value == 0LL) && (text != "0")) {
        return false;
    }
    out = static_cast<std::int64_t>(value);
    return true;
}

bool listSubdirectories(const std::string& dir, std::vector<std::string>& out)
{
    DIR* handle = opendir(dir.c_str());
    if (handle == nullptr) {
        return false;
    }
    out.clear();
    while (const dirent* entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if ((name == ".") || (name == "..")) {
            continue;
        }
        struct stat info;
        const std::string full = dir + "/" + name;
        if ((stat(full.c_str(), &info) == 0) && S_ISDIR(info.st_mode)) {
            out.push_back(name);
        }
    }
    closedir(handle);
    return true;
}

} // namespace gw::sensors
