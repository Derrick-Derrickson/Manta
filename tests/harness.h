// A minimal test harness.
//
// Deliberately not a third-party framework: it is ~80 lines, needs no network
// at configure time, and cross-compiles wherever the compiler does. Failures
// print file:line and the actual values, which is all a golden-file suite needs.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mantatest {

struct Case {
    std::string_view name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failureCount() {
    static int n = 0;
    return n;
}

inline std::string_view& currentCase() {
    static std::string_view name;
    return name;
}

struct Registrar {
    Registrar(std::string_view name, std::function<void()> fn) {
        registry().push_back(Case{name, std::move(fn)});
    }
};

inline void fail(const char* file, int line, const std::string& what) {
    ++failureCount();
    std::fprintf(stderr, "  FAIL %s:%d\n    in case: %s\n    %s\n", file, line,
                 std::string(currentCase()).c_str(), what.c_str());
}

template <typename A, typename B>
void checkEq(const char* file, int line, const char* ea, const char* eb, const A& a, const B& b) {
    if (!(a == b)) {
        std::string sa, sb;
        if constexpr (std::is_convertible_v<A, std::string_view>) sa = std::string(std::string_view(a));
        else sa = std::to_string(a);
        if constexpr (std::is_convertible_v<B, std::string_view>) sb = std::string(std::string_view(b));
        else sb = std::to_string(b);
        fail(file, line, std::string(ea) + " == " + eb + "\n      lhs: " + sa + "\n      rhs: " + sb);
    }
}

inline int runAll(int argc, char** argv) {
    std::string_view filter = argc > 1 ? argv[1] : std::string_view{};
    int ran = 0;
    for (auto& c : registry()) {
        if (!filter.empty() && c.name.find(filter) == std::string_view::npos) continue;
        currentCase() = c.name;
        int before = failureCount();
        c.fn();
        ++ran;
        if (failureCount() != before) std::fprintf(stderr, "  [FAILED] %s\n", std::string(c.name).c_str());
    }
    std::fprintf(stderr, "%d case(s) run, %d failure(s)\n", ran, failureCount());
    return failureCount() == 0 ? 0 : 1;
}

}  // namespace mantatest

// Two levels of indirection so that __LINE__ expands before pasting; without it
// every case in a file would define the same function name.
#define MANTA_PASTE_(a, b) a##b
#define MANTA_PASTE(a, b) MANTA_PASTE_(a, b)

#define TEST_CASE(name)                                                            \
    static void MANTA_PASTE(test_fn_, __LINE__)();                                 \
    static ::mantatest::Registrar MANTA_PASTE(test_reg_, __LINE__)(                \
        name, MANTA_PASTE(test_fn_, __LINE__));                                    \
    static void MANTA_PASTE(test_fn_, __LINE__)()

#define CHECK_EQ(a, b) ::mantatest::checkEq(__FILE__, __LINE__, #a, #b, (a), (b))

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) ::mantatest::fail(__FILE__, __LINE__, "expected: " #cond); \
    } while (0)

#define CHECK_FALSE(cond)                                                      \
    do {                                                                       \
        if (cond) ::mantatest::fail(__FILE__, __LINE__, "expected false: " #cond); \
    } while (0)

#define TEST_MAIN() \
    int main(int argc, char** argv) { return ::mantatest::runAll(argc, argv); }
