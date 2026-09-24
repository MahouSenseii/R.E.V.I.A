#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace revia::improvement
{

// Her own source, as far as reviewing it goes: which files she may read and propose
// changes to, and which of them relate to a problem.
//
// Reviewable means the product code -- Private/, Public/, Desktop/ and the Python voice
// worker in Tools/. Deliberately not the tests, the build files, the scripts, or the
// configuration: a proposal proven by the tests must not be able to change the tests, the
// build that runs them, or the settings and scripts that decide what she may do.
class SourceCatalog
{
public:
    SourceCatalog() = default;
    explicit SourceCatalog(std::filesystem::path root);

    // The repository she was built from: the first directory at or above `start` that
    // holds CMakeLists.txt, Private/, and Public/. Empty when she is running from an
    // install with no source beside it, which simply turns the feature off.
    [[nodiscard]] static std::optional<std::filesystem::path> LocateSourceRoot(
        const std::filesystem::path& start);

    // A relative path, forward slashes, inside the reviewable set. The only gate a path
    // from a model reply is let through.
    [[nodiscard]] static bool IsReviewable(const std::string& relativePath);

    [[nodiscard]] const std::filesystem::path& Root() const { return root; }
    [[nodiscard]] bool Valid() const { return !root.empty(); }

    // Every reviewable file, sorted.
    [[nodiscard]] std::vector<std::string> Files() const;

    // Bounded read of a reviewable file. False, with a reason, for anything else.
    [[nodiscard]] bool Read(
        const std::string& relativePath, std::string& outContent, std::string& outError) const;

    // The files behind a named component ("Qwen3-TTS", "Resource planner") or a metric
    // ("qwen_synthesis"), best first. Component names match file names; metric names
    // match the code that records them.
    [[nodiscard]] std::vector<std::string> FilesFor(
        const std::vector<std::string>& components,
        const std::vector<std::string>& metrics,
        std::size_t maximum = 4) const;

    // A request in plain words -- a path, a file name, or a component.
    [[nodiscard]] std::vector<std::string> Resolve(const std::string& target) const;

private:
    std::filesystem::path root;
};

// A slice of one file small enough for the review prompt.
struct CodeWindow
{
    std::string path;
    // 1-based, inclusive.
    std::size_t firstLine = 0;
    std::size_t lastLine = 0;
    std::size_t totalLines = 0;
    std::string text;
};

// At most `maximumLines` lines, and never more than `maximumCharacters`. Centred on the
// first line mentioning one of `anchors` when any does; otherwise starting at `startLine`.
// The window is widened to whole lines and never splits one.
[[nodiscard]] CodeWindow ExtractWindow(
    const std::string& path,
    const std::string& content,
    const std::vector<std::string>& anchors,
    std::size_t startLine,
    std::size_t maximumLines,
    std::size_t maximumCharacters);

} // namespace revia::improvement
