// A deliberately small test framework: a registry plus three assertion macros.
// Keeping it in the repository means the project still has zero dependencies.
#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const std::string& name, std::function<void()> body);
};

void fail(const char* file, int line, const std::string& message);

int run_all();

}  // namespace testing

#define TEST(suite, name)                                                       \
    static void suite##_##name##_body();                                        \
    static ::testing::Registrar suite##_##name##_registrar(#suite "." #name,    \
                                                           suite##_##name##_body); \
    static void suite##_##name##_body()

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            ::testing::fail(__FILE__, __LINE__, "expected true: " #condition);    \
        }                                                                         \
    } while (false)

#define CHECK_EQ(actual, expected)                                                \
    do {                                                                          \
        const auto actual_value = (actual);                                       \
        const auto expected_value = (expected);                                   \
        if (!(actual_value == expected_value)) {                                  \
            std::ostringstream message;                                           \
            message << #actual << " == " #expected << "\n  actual:   "            \
                    << actual_value << "\n  expected: " << expected_value;        \
            ::testing::fail(__FILE__, __LINE__, message.str());                   \
        }                                                                         \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                   \
    do {                                                                          \
        const double actual_value = (actual);                                     \
        const double expected_value = (expected);                                 \
        const double tolerance_value = (tolerance);                               \
        if (!(std::fabs(actual_value - expected_value) <= tolerance_value)) {     \
            std::ostringstream message;                                           \
            message << #actual << " is not within " << tolerance_value << " of "  \
                    << #expected << "\n  actual:   " << actual_value              \
                    << "\n  expected: " << expected_value;                        \
            ::testing::fail(__FILE__, __LINE__, message.str());                   \
        }                                                                         \
    } while (false)

// Calls `expression` and passes when it throws something whose what() contains
// `needle`. Any other outcome is a failure.
#define CHECK_THROWS_AS(expression, needle)                                       \
    do {                                                                          \
        bool caught = false;                                                      \
        try {                                                                     \
            expression;                                                           \
        } catch (const std::exception& error) {                                   \
            caught = true;                                                        \
            const std::string what = error.what();                                \
            if (what.find(needle) == std::string::npos) {                         \
                ::testing::fail(__FILE__, __LINE__,                               \
                                std::string("exception message '") + what +        \
                                    "' does not contain '" + (needle) + "'");      \
            }                                                                     \
        }                                                                         \
        if (!caught) {                                                            \
            ::testing::fail(__FILE__, __LINE__, "expected an exception");          \
        }                                                                         \
    } while (false)
