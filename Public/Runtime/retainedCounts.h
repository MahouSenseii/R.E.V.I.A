#pragma once

#include <vector>

namespace revia::runtime
{

// Counts only the warnings and errors still retained in a bounded log.
struct RetainedSeverityCounts
{
    int warnings = 0;
    int errors = 0;
};

// Severity 1 is a warning, 2 an error; other values are informational.
[[nodiscard]] RetainedSeverityCounts CountRetainedSeverities(const std::vector<int>& severities);

} // namespace revia::runtime
