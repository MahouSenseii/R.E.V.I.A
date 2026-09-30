#include "Runtime/retainedCounts.h"

namespace revia::runtime
{

RetainedSeverityCounts CountRetainedSeverities(const std::vector<int>& severities)
{
    RetainedSeverityCounts counts;
    for (const int severity : severities)
    {
        if (severity == 1) ++counts.warnings;
        else if (severity == 2) ++counts.errors;
    }
    return counts;
}

} // namespace revia::runtime
