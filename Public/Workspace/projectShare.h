#pragma once

#include "Workspace/guestTypes.h"

#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::workspace
{

struct ProjectShareLimits
{
    std::size_t maxFiles = 2000;
    // Per file; the guest write limit applies as well.
    std::size_t maxFileBytes = 65536;
    std::size_t maxTotalBytes = 8u << 20;
    // Directory entries looked at, shared or not, so a huge tree cannot stall the turn.
    std::size_t maxEntries = 50000;
    // Escaped size of one batch. The bridge re-encodes requests as ASCII JSON under 512 KiB.
    std::size_t maxBatchBytes = 300000;
};

// The text files of one host folder, read once by the host and grouped into guest import
// batches. Nothing is mounted: the guest receives these copies and nothing else.
struct ProjectSharePlan
{
    std::vector<std::vector<GuestFile>> batches;
    std::size_t files = 0;
    std::size_t bytes = 0;
    // "path: reason" for files left out, bounded; `skippedCount` counts all of them.
    std::vector<std::string> skipped;
    std::size_t skippedCount = 0;
    // Set when the folder cannot be shared at all; the plan is then empty.
    std::string error;
};

// Walks `folder` without following links or reparse points. Leaves out version-control and
// build folders, files that are not UTF-8 text, files over the size limit, and files whose
// name or content looks like a credential. Fails as a whole when the folder holds more than
// the limits allow, so a share is never silently partial.
[[nodiscard]] ProjectSharePlan PlanProjectShare(const std::filesystem::path& folder, const ProjectShareLimits& limits, std::stop_token stop = {});

} // namespace revia::workspace
