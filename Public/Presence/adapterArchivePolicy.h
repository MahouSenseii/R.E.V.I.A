#pragma once

#include <chrono>
#include <filesystem>
#include <utility>
#include <vector>

namespace revia::presence
{

// One archived adapter envelope: when it was written, and where it is.
using ArchivedFile =
    std::pair<std::filesystem::file_time_type, std::filesystem::path>;

// Applies archive age expiry first, then the count limit to surviving envelopes.
// Each nonpositive limit is disabled; both disabled retains everything.
[[nodiscard]] std::vector<std::filesystem::path> SelectExpiredArchiveFiles(std::vector<ArchivedFile> files,
    int maximumFiles, int maximumAgeDays, std::filesystem::file_time_type now);

} // namespace revia::presence
