#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace revia::perception
{
struct SavedFileObservation
{
    std::filesystem::path projectRoot;
    std::filesystem::path path;
    std::string revision;
    std::string contents;
    std::chrono::system_clock::time_point occurredAt;
};

// The session supplies policy-approved canonical paths and executor-read bytes.
// Validation establishes a bounded saved snapshot, never code correctness or authorship.
[[nodiscard]] bool ValidateSavedFileObservation(const SavedFileObservation& observation,
    std::chrono::system_clock::time_point now, std::string& reason);
[[nodiscard]] std::string SavedFileContext(const SavedFileObservation& observation);
}
