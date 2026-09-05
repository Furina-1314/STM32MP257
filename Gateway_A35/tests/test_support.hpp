#ifndef GW_TESTS_TEST_SUPPORT_HPP
#define GW_TESTS_TEST_SUPPORT_HPP

#include <cstdio>
#include <string>

// Minimal dependency-free test harness: each test binary defines main(),
// uses CHECK/CHECK_EQ, and ends with TEST_MAIN_END (exit code 0 iff all
// checks passed).

namespace test {

inline int& failureCount()
{
    static int failures = 0;
    return failures;
}

inline int& checkCount()
{
    static int checks = 0;
    return checks;
}

inline void report(const char* file, int line, const std::string& what)
{
    ++failureCount();
    std::printf("  FAIL %s:%d: %s\n", file, line, what.c_str());
}

} // namespace test

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++test::checkCount();                                                \
        if (!(cond)) {                                                       \
            test::report(__FILE__, __LINE__, #cond);                         \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++test::checkCount();                                                \
        const auto& va_ = (a);                                               \
        const auto& vb_ = (b);                                               \
        if (!(va_ == vb_)) {                                                 \
            test::report(__FILE__, __LINE__,                                 \
                         std::string(#a " == ") + #b                         \
                                 + " (actual differ)");                      \
        }                                                                    \
    } while (0)

#define TEST_MAIN_END                                                        \
    do {                                                                     \
        std::printf("%s: %d checks, %d failures\n", __FILE__,                \
                    test::checkCount(), test::failureCount());               \
        return (test::failureCount() == 0) ? 0 : 1;                          \
    } while (0)

#endif // GW_TESTS_TEST_SUPPORT_HPP
