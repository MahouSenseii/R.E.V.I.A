#include "Presence/adapterArchivePolicy.h"

#include <algorithm>

namespace revia::presence
{

std::vector<std::filesystem::path> SelectExpiredArchiveFiles(
    std::vector<ArchivedFile> files,
    const int maximumFiles,
    const int maximumAgeDays,
    const std::filesystem::file_time_type now)
{
    std::vector<std::filesystem::path> expired;
    // Oldest first, so both rules below read in the same direction.
    std::sort(files.begin(), files.end(),
        [](const ArchivedFile& left, const ArchivedFile& right)
        {
            return left.first < right.first;
        });

    std::vector<ArchivedFile> surviving;
    surviving.reserve(files.size());
    if (maximumAgeDays > 0)
    {
        const auto cutoff = now - std::chrono::hours(24 * maximumAgeDays);
        for (ArchivedFile& file : files)
        {
            if (file.first < cutoff) expired.push_back(file.second);
            else surviving.push_back(std::move(file));
        }
    }
    else
    {
        surviving = std::move(files);
    }

    if (maximumFiles > 0 &&
        surviving.size() > static_cast<std::size_t>(maximumFiles))
    {
        const std::size_t excess =
            surviving.size() - static_cast<std::size_t>(maximumFiles);
        for (std::size_t index = 0; index < excess; ++index)
        {
            expired.push_back(surviving[index].second);
        }
    }
    return expired;
}

} // namespace revia::presence
