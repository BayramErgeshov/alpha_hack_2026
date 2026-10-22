#pragma once
// Minimal test harness: no third-party dependencies, works with CTest.
#include <functional>
#include <iostream>
#include <string>
#include <vector>

struct TestCase { std::string name; std::function<void()> body; };
inline std::vector<TestCase>& test_registry() { static std::vector<TestCase> r; return r; }
struct TestRegistrar { TestRegistrar(const char* n, std::function<void()> f) { test_registry().push_back({ n, std::move(f) }); } };
struct TestFailure { std::string message; };

#define TEST(name) static void name(); static TestRegistrar name##_registrar(#name, name); static void name()
#define CHECK(cond) do { if (!(cond)) throw TestFailure{ std::string(__FILE__) + ":" + std::to_string(__LINE__) + " CHECK(" #cond ")" }; } while (0)

inline int run_all_tests(const std::string& filter) {
    int failed = 0, ran = 0;
    for (const auto& t : test_registry()) {
        if (!filter.empty() && t.name.find(filter) == std::string::npos) continue;
        ++ran;
        try { t.body(); std::cout << "[ OK ] " << t.name << "\n"; }
        catch (const TestFailure& f) { ++failed; std::cout << "[FAIL] " << t.name << ": " << f.message << "\n"; }
        catch (const std::exception& e) { ++failed; std::cout << "[FAIL] " << t.name << ": exception " << e.what() << "\n"; }
    }
    std::cout << (ran - failed) << "/" << ran << " tests passed\n";
    return failed == 0 && ran > 0 ? 0 : 1;
}
