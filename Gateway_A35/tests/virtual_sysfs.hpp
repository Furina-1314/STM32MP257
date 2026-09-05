#ifndef GW_TESTS_VIRTUAL_SYSFS_HPP
#define GW_TESTS_VIRTUAL_SYSFS_HPP

// Virtual sysfs tree for sensor reader tests: real directories and files on
// the host, rooted in a unique temp path. Readers get sysfsRoot pointed at
// it; tests mutate the tree to simulate plug/unplug and renumbering.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#define GW_MKDIR(path) _mkdir(path)
#else
#define GW_MKDIR(path) mkdir(path, 0755)
#endif

namespace gw {

class VirtualSysfs
{
public:
    VirtualSysfs()
    {
        static int instance = 0; // distinct roots per instance in-process
#ifdef _WIN32
        // Native MinGW binaries do not understand the MSYS /tmp mapping:
        // root the virtual tree under the Windows TEMP directory.
        const char* base = std::getenv("TEMP");
        if ((base == nullptr) || (*base == '\0')) {
            base = ".";
        }
        root_ = std::string(base);
        for (char& c : root_) {
            if (c == '\\') {
                c = '/';
            }
        }
        root_ += "/gw_vsfs_" + std::to_string(getpid()) + "_"
                 + std::to_string(instance++);
#else
        char base[] = "/tmp/gw_vsfs_XXXXXX";
        const char* made = mkdtemp(base);
        root_ = (made != nullptr) ? made : "/tmp/gw_vsfs_fallback";
#endif
        makeDirs(join("")); // create the root (and parents) if needed
    }

    ~VirtualSysfs() { removeTree(root_); }

    const std::string& root() const { return root_; }

    // Creates parent directories as needed, then writes the file (trunc).
    void writeFile(const std::string& relative, const std::string& content)
    {
        const std::string path = join(relative);
        const std::string::size_type slash = path.find_last_of('/');
        if (slash != std::string::npos) {
            makeDirs(path.substr(0U, slash));
        }
        FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr) {
            return;
        }
        std::fwrite(content.data(), 1U, content.size(), file);
        std::fclose(file);
    }

    void removeFile(const std::string& relative)
    {
        std::remove(join(relative).c_str());
    }

    void removeDir(const std::string& relative)
    {
        rmdir(join(relative).c_str());
    }

private:
    std::string join(const std::string& relative) const
    {
        if (relative.empty() || (relative[0] == '/')) {
            return root_ + relative;
        }
        return root_ + "/" + relative;
    }

    void makeDirs(const std::string& path)
    {
        // Windows TEMP may carry backslashes: normalize first so a single
        // '/'-split walks the whole path, and seed `built` so the first
        // segment never gains a bogus leading slash ("C:" stays "C:",
        // "/tmp" stays "/tmp").
        std::string built;
        if (!path.empty() && (path[0] == '/')) {
            built = "/";
        }
        std::string::size_type pos = 0U;
        while (pos <= path.size()) {
            const std::string::size_type next = path.find('/', pos);
            const std::string segment =
                    path.substr(pos, (next == std::string::npos)
                                             ? std::string::npos
                                             : next - pos);
            if (!segment.empty()) {
                if (built.empty() || (built == "/")) {
                    built += segment;
                } else {
                    built += "/" + segment;
                }
                GW_MKDIR(built.c_str()); // EEXIST is fine
            }
            if (next == std::string::npos) {
                break;
            }
            pos = next + 1U;
        }
    }

    static void removeTree(const std::string& path)
    {
        // Shallow recursion over a tiny test tree.
        DIR* dir = opendir(path.c_str());
        if (dir != nullptr) {
            while (const dirent* entry = readdir(dir)) {
                const std::string name = entry->d_name;
                if ((name == ".") || (name == "..")) {
                    continue;
                }
                struct stat info;
                const std::string child = path + "/" + name;
                if ((stat(child.c_str(), &info) == 0) && S_ISDIR(info.st_mode)) {
                    removeTree(child);
                } else {
                    std::remove(child.c_str());
                }
            }
            closedir(dir);
        }
        rmdir(path.c_str());
    }

    std::string root_;
};

} // namespace gw

#endif // GW_TESTS_VIRTUAL_SYSFS_HPP
