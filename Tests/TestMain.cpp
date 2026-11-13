#include "TestFramework.h"

#include <chrono>
#include <cstring>

int main (int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (auto& t : tf::registry())
    {
        if (filter != nullptr && t.name.find (filter) == std::string::npos) continue;
        const int before = tf::failures();
        std::printf ("[ RUN  ] %s\n", t.name.c_str());
        const auto t0 = std::chrono::steady_clock::now();
        t.fn();
        const auto ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("[ %s ] %s (%.0f ms)\n", tf::failures() == before ? " OK " : "FAIL", t.name.c_str(), ms);
        ++run;
    }
    std::printf ("\n%d tests, %d failed checks\n", run, tf::failures());
    return tf::failures() == 0 ? 0 : 1;
}
