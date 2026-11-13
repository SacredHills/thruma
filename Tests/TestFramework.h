// Minimal self-contained test framework (no external dependency).
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace tf
{

struct TestCase
{
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> r;
    return r;
}

inline int& failures()
{
    static int f = 0;
    return f;
}

struct Registrar
{
    Registrar (const char* name, std::function<void()> fn) { registry().push_back ({ name, std::move (fn) }); }
};

inline void report (bool ok, const char* expr, const char* file, int line, const std::string& msg = {})
{
    if (! ok)
    {
        ++failures();
        std::printf ("    FAILED: %s  (%s:%d) %s\n", expr, file, line, msg.c_str());
    }
}

/** Print a metric so CI logs show the measured quality numbers, not just pass/fail. */
inline void metric (const char* name, double value, const char* unit = "")
{
    std::printf ("    %-48s %10.3f %s\n", name, value, unit);
}

} // namespace tf

#define TF_CONCAT2(a, b) a##b
#define TF_CONCAT(a, b) TF_CONCAT2 (a, b)
#define TEST_CASE(name) \
    static void TF_CONCAT (tfTest_, __LINE__)(); \
    static tf::Registrar TF_CONCAT (tfReg_, __LINE__) (name, &TF_CONCAT (tfTest_, __LINE__)); \
    static void TF_CONCAT (tfTest_, __LINE__)()

#define CHECK(expr) tf::report ((expr), #expr, __FILE__, __LINE__)
#define CHECK_MSG(expr, msg) tf::report ((expr), #expr, __FILE__, __LINE__, (msg))
#define CHECK_NEAR(a, b, tol) \
    tf::report (std::abs ((double) (a) - (double) (b)) <= (double) (tol), #a " ~= " #b, __FILE__, __LINE__, \
                "(" + std::to_string ((double) (a)) + " vs " + std::to_string ((double) (b)) + ")")
