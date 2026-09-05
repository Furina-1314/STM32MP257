// IniConfig: sections, comments, trimming, typed getters, defaults on
// missing keys/files, malformed-line tolerance. Values are taken verbatim
// between '=' and end-of-line (no inline comments, matching the documented
// config format).
#include "test_support.hpp"

#include "util/ini_config.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

const char* kSample = R"INI(; leading comment
[tcp]
port = 7001
bind = 0.0.0.0
client_policy = reject

[gateway]
safe_limit_pct = 45
stop_on_disconnect = false
heartbeat_timeout_ms = 0

[log]
level = warning
)INI";

struct TempFile
{
    std::string file;
    TempFile()
    {
#ifdef _WIN32
        const char* base = std::getenv("TEMP");
        file = std::string(base ? base : ".") + "/gw_ini_test.ini";
#else
        file = "/tmp/gw_ini_test.ini";
#endif
    }
    ~TempFile() { std::remove(file.c_str()); }

    void write(const char* content)
    {
        FILE* f = std::fopen(file.c_str(), "wb");
        std::fputs(content, f);
        std::fclose(f);
    }
};

} // namespace

int main()
{
    using gw::IniConfig;

    TempFile tf; // owns the file for the whole test body below
    tf.write(kSample);
    IniConfig ini;
    CHECK(ini.load(tf.file));
    CHECK_EQ(ini.size(), static_cast<std::size_t>(7U)); // 3+3+1 keys

    CHECK_EQ(ini.get("tcp", "port", "7000"), std::string("7001"));
    CHECK_EQ(ini.getInt("tcp", "port", 7000), 7001);
    CHECK_EQ(ini.get("tcp", "bind", "x"), std::string("0.0.0.0"));
    CHECK_EQ(ini.get("tcp", "client_policy", "takeover"),
             std::string("reject"));
    CHECK_EQ(ini.getInt("gateway", "safe_limit_pct", 30), 45);
    CHECK(!ini.getBool("gateway", "stop_on_disconnect", true));
    CHECK_EQ(ini.getInt("gateway", "heartbeat_timeout_ms", 5000), 0);
    CHECK_EQ(ini.get("log", "level", "info"), std::string("warning"));

    // Defaults for missing keys and sections.
    CHECK_EQ(ini.get("nope", "key", "fallback"), std::string("fallback"));
    CHECK_EQ(ini.getInt("nope", "key", 42), 42);
    CHECK(ini.getBool("nope", "key", true));
    CHECK(!ini.has("nope", "key"));
    CHECK(ini.has("tcp", "port"));

    // Malformed / empty content tolerated.
    {
        tf.write("# comment only\n\n[broke]\nno_equals_sign\n=noname\n"
                 "[empty]\n");
        IniConfig bad;
        CHECK(bad.load(tf.file));
        CHECK_EQ(bad.size(), static_cast<std::size_t>(0U));
        CHECK_EQ(bad.getInt("broke", "x", 7), 7);
    }

    // Non-numeric values fall back; booleans accept word forms.
    {
        tf.write("[a]\nv = 12abc\nb = true\nc = off\n");
        IniConfig c;
        CHECK(c.load(tf.file));
        CHECK_EQ(c.getInt("a", "v", 5), 5);
        CHECK(c.getBool("a", "b", false));
        CHECK(!c.getBool("a", "c", true));
    }

    // Missing file: load fails, defaults serve.
    {
        IniConfig missing;
        CHECK(!missing.load("/nonexistent/gw.ini"));
        CHECK_EQ(missing.get("x", "y", "d"), std::string("d"));
    }

    TEST_MAIN_END;
}
