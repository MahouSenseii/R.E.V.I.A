#pragma once

#include "Learning/datasetExport.h"
#include "Learning/liveCapture.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace revia::learning
{

// One in-app export of the owner's corrected live captures.
struct LiveDatasetExport
{
    // The numbered dataset name, such as revia-live-3, and the folder it was written to.
    std::string name;
    std::uint64_t number = 0;
    std::filesystem::path directory;
    std::string manifestSha256;
    std::size_t train = 0;
    std::size_t validation = 0;
    std::size_t test = 0;
    // Kept captures with no decision yet; they are not part of the export.
    std::size_t waiting = 0;
    std::vector<DatasetDecision> excluded;
    // The companion's withdrawal list, which a training job names as withdrawnSourcesPath.
    std::filesystem::path withdrawnList;
};

inline constexpr unsigned LiveValidationPercent = 10;
inline constexpr unsigned LiveTestPercent = 10;
// The evaluation plan's default minimum of held-out cases.
inline constexpr std::size_t LiveMinimumTestCases = 5;

// Exports every decided live capture into a new numbered folder under `parent`, as
// revia_dataset_capture.py candidates and ReviaDatasetExport would, with the companion's
// withdrawal list applied. The lineage under `lineageRoot` keeps one salt and a copy of every
// earlier manifest, so an example once placed in validation or test never moves into
// training. Writes nothing when no accepted correction is eligible.
bool ExportLiveDataset(const LiveCaptureStore& store, const std::filesystem::path& lineageRoot, const std::string& lineageName,
    const std::filesystem::path& parent, LiveDatasetExport& outExport, std::string& outError);

// Why `folder` cannot hold private training data, or nothing. It must be an existing local
// folder given in full, with no link or reparse point on its path, and outside OneDrive,
// which would upload it.
[[nodiscard]] std::string PrivateDataFolderRefusal(const std::filesystem::path& folder);

// What the owner is told about a finished export.
[[nodiscard]] std::string DescribeLiveDatasetExport(const LiveDatasetExport& value);

} // namespace revia::learning
