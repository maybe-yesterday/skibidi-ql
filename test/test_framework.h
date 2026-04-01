#pragma once
// Minimal single-header test framework - no external dependencies.
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>

// -----------------------------------------------------------------------
// Test registry
// -----------------------------------------------------------------------
struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> r;
    return r;
}

struct TestRegistrar {
    TestRegistrar(const char* name, std::function<void()> fn) {
        test_registry().push_back({name, std::move(fn)});
    }
};

// Declare a test. Usage:
//   TEST(my_test_name) { ... assertions ... }
#define TEST(name)                                              \
    static void _test_body_##name();                           \
    static TestRegistrar _reg_##name(#name, _test_body_##name);\
    static void _test_body_##name()

// -----------------------------------------------------------------------
// Assertion macros
// -----------------------------------------------------------------------
#define ASSERT_TRUE(cond)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::ostringstream _ss;                                           \
            _ss << __FILE__ << ":" << __LINE__                               \
                << ": ASSERT_TRUE failed: " #cond;                           \
            throw std::runtime_error(_ss.str());                             \
        }                                                                     \
    } while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b)                                                       \
    do {                                                                      \
        auto&& _a = (a);                                                      \
        auto&& _b = (b);                                                      \
        if (!(_a == _b)) {                                                    \
            std::ostringstream _ss;                                           \
            _ss << __FILE__ << ":" << __LINE__                               \
                << ": ASSERT_EQ failed: " #a " == " #b                      \
                << " (" << _a << " != " << _b << ")";                       \
            throw std::runtime_error(_ss.str());                             \
        }                                                                     \
    } while (0)

#define ASSERT_NE(a, b)                                                       \
    do {                                                                      \
        auto&& _a = (a);                                                      \
        auto&& _b = (b);                                                      \
        if (_a == _b) {                                                       \
            std::ostringstream _ss;                                           \
            _ss << __FILE__ << ":" << __LINE__                               \
                << ": ASSERT_NE failed: " #a " != " #b " (both equal)";     \
            throw std::runtime_error(_ss.str());                             \
        }                                                                     \
    } while (0)

#define ASSERT_THROW(expr, ExcType)                                           \
    do {                                                                      \
        bool _threw = false;                                                  \
        try { (void)(expr); }                                                 \
        catch (const ExcType&) { _threw = true; }                            \
        catch (...) {}                                                        \
        if (!_threw) {                                                        \
            std::ostringstream _ss;                                           \
            _ss << __FILE__ << ":" << __LINE__                               \
                << ": Expected exception " #ExcType " from: " #expr;        \
            throw std::runtime_error(_ss.str());                             \
        }                                                                     \
    } while (0)

#define ASSERT_NO_THROW(expr)                                                 \
    do {                                                                      \
        try { (void)(expr); }                                                 \
        catch (const std::exception& _e) {                                   \
            std::ostringstream _ss;                                           \
            _ss << __FILE__ << ":" << __LINE__                               \
                << ": Unexpected exception: " << _e.what();                  \
            throw std::runtime_error(_ss.str());                             \
        }                                                                     \
    } while (0)

// Check that a string contains a substring
#define ASSERT_CONTAINS(haystack, needle)                                     \
    do {                                                                      \
        std::string _h = (haystack);                                          \
        std::string _n = (needle);                                            \
        if (_h.find(_n) == std::string::npos) {                              \
            std::ostringstream _ss;                                           \
            _ss << __FILE__ << ":" << __LINE__                               \
                << ": ASSERT_CONTAINS: \"" << _n                             \
                << "\" not found in \"" << _h << "\"";                      \
            throw std::runtime_error(_ss.str());                             \
        }                                                                     \
    } while (0)

// -----------------------------------------------------------------------
// Runner
// -----------------------------------------------------------------------
inline int run_all_tests(const std::string& suite_name = "") {
    int passed = 0, failed = 0;
    for (auto& tc : test_registry()) {
        std::cout << "[RUN ] " << tc.name << "\n";
        try {
            tc.fn();
            std::cout << "[PASS] " << tc.name << "\n";
            ++passed;
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << tc.name << ": " << e.what() << "\n";
            ++failed;
        }
    }
    std::cout << "\n";
    if (!suite_name.empty()) std::cout << suite_name << " - ";
    std::cout << passed << " passed, " << failed << " failed\n";
    return (failed > 0) ? 1 : 0;
}
