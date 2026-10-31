#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>

// The desktop's Qt checks share the ReviaDesktopStopTests binary, which already compiles every
// ReviaDesktop source, so a new suite costs no extra executable and no extra link. A suite
// registers itself from its own source file with REVIA_DESKTOP_SUITE; main() in
// desktopStopTests.cpp sets up the QApplication, temporary QSettings and working directory, then
// runs the suite named by --suite=<name> ("policy" when none is given).
namespace revia::tests
{
// Returns the failure count, or SuiteSkipped. argc/argv are what QApplication left after taking
// its own options.
using DesktopSuite = std::function<int(int argc, char** argv)>;

// Returned only after a suite's own checks passed and a local prerequisite (such as FFmpeg) is
// missing. main() reports it as CTest's skip code 77. It is negative, so no failure count,
// however large, can be taken for a skip.
inline constexpr int SuiteSkipped = -77;

inline std::map<std::string, DesktopSuite>& DesktopSuites()
{
    static std::map<std::string, DesktopSuite> suites;
    return suites;
}

struct DesktopSuiteRegistration
{
    DesktopSuiteRegistration(const char* name, DesktopSuite run)
    {
        DesktopSuites()[name] = std::move(run);
    }
};
}

#define REVIA_DESKTOP_SUITE(name, fn) static const ::revia::tests::DesktopSuiteRegistration reviaDesktopSuite_##fn(name, fn)
