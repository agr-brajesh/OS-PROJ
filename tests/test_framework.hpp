#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <functional>
#include <sstream>

namespace test {

struct TestCase {
    std::string name;
    std::function<void()> func;
};

class TestRegistry {
public:
    static TestRegistry& instance() {
        static TestRegistry reg;
        return reg;
    }

    void register_test(const std::string& name, std::function<void()> func) {
        tests_.push_back({name, func});
    }

    int run_all() {
        int passed = 0;
        int failed = 0;
        std::cout << "\033[1;34m=== Running Unit Tests (" << tests_.size() << " tests) ===\033[0m\n\n";

        for (const auto& test : tests_) {
            std::cout << "  [ RUN      ] " << test.name << std::endl;
            try {
                test.func();
                std::cout << "  \033[0;32m[       OK ]\033[0m " << test.name << std::endl;
                ++passed;
            } catch (const std::exception& e) {
                std::cout << "  \033[0;31m[  FAILED  ]\033[0m " << test.name << " (Error: " << e.what() << ")\n";
                ++failed;
            } catch (...) {
                std::cout << "  \033[0;31m[  FAILED  ]\033[0m " << test.name << " (Unknown exception)\n";
                ++failed;
            }
        }

        std::cout << "\n\033[1m=== Test Summary ===\033[0m\n";
        std::cout << "  Total:  " << tests_.size() << "\n";
        std::cout << "  \033[0;32mPassed: " << passed << "\033[0m\n";
        if (failed > 0) {
            std::cout << "  \033[0;31mFailed: " << failed << "\033[0m\n";
            return 1;
        }
        std::cout << "  \033[0;32mAll tests passed successfully!\033[0m\n";
        return 0;
    }

private:
    std::vector<TestCase> tests_;
};

struct TestRegistrar {
    TestRegistrar(const std::string& name, std::function<void()> func) {
        TestRegistry::instance().register_test(name, func);
    }
};

class AssertionFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace test

#define TEST_CASE(name) \
    static void test_func_##name(); \
    static test::TestRegistrar test_reg_##name(#name, test_func_##name); \
    static void test_func_##name()

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        std::ostringstream _oss; \
        _oss << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__; \
        throw test::AssertionFailure(_oss.str()); \
    } \
} while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b) do { \
    if (!((a) == (b))) { \
        std::ostringstream _oss; \
        _oss << "Assertion failed: (" #a " == " #b ") [" << (a) << " != " << (b) << "] at " << __FILE__ << ":" << __LINE__; \
        throw test::AssertionFailure(_oss.str()); \
    } \
} while(0)

#define ASSERT_NE(a, b) do { \
    if ((a) == (b)) { \
        std::ostringstream _oss; \
        _oss << "Assertion failed: (" #a " != " #b ") [" << (a) << " == " << (b) << "] at " << __FILE__ << ":" << __LINE__; \
        throw test::AssertionFailure(_oss.str()); \
    } \
} while(0)

#define ASSERT_NEAR(a, b, eps) do { \
    if (std::abs((a) - (b)) > (eps)) { \
        std::ostringstream _oss; \
        _oss << "Assertion failed: |" #a " - " #b "| <= " #eps " [|" << (a) << " - " << (b) << "| = " << std::abs((a) - (b)) << " > " << (eps) << "] at " << __FILE__ << ":" << __LINE__; \
        throw test::AssertionFailure(_oss.str()); \
    } \
} while(0)

