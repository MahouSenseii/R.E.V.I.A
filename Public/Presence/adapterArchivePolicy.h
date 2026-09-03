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

// Which archived envelopes to delete, given the retention limits.
//
// Separated from the directory walk so the policy is testable without a filesystem, and
// so the two limits stay in one place. Age is applied first and the count limit to what
// survives it, because an old file is expired whether or not the directory is full.
//
// A limit of zero or less means that limit is off. Both off keeps everything, which is
// the previous behaviour and is still a legitimate configuration -- it just has to be
// chosen rather than be the only option.
[[nodiscard]] std::vector<std::filesystem::path> SelectExpiredArchiveFiles(
    std::vector<ArchivedFile> files,
    int maximumFiles,
    int maximumAgeDays,
    std::filesystem::file_time_type now);

} // namespace revia::presence
