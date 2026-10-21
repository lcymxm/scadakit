#include "test_framework.hpp"

#include <exception>

namespace testing {
namespace {

int checks = 0;
std::string current_test;
bool current_failed = false;

}  // namespace

std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

Registrar::Registrar(const std::string& name, std::function<void()> body) {
    registry().push_back(TestCase{name, std::move(body)});
}

void fail(const char* file, int line, const std::string& message) {
    ++checks;
    current_failed = true;
    std::cout << "  " << current_test << ": " << file << ":" << line << ": " << message << "\n";
}

int run_all() {
    int passed = 0;
    int failed = 0;
    for (const TestCase& test : registry()) {
        current_test = test.name;
        current_failed = false;
        try {
            test.body();
        } catch (const std::exception& error) {
            current_failed = true;
            std::cout << "  " << test.name << ": threw: " << error.what() << "\n";
        } catch (...) {
            current_failed = true;
            std::cout << "  " << test.name << ": threw an unknown exception\n";
        }
        if (current_failed) {
            std::cout << "  [ FAIL ] " << test.name << "\n";
            ++failed;
        } else {
            ++passed;
        }
    }
    ++checks;
    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace testing

int main() { return testing::run_all(); }
