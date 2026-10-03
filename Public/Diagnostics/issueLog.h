#pragma once

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace revia::diagnostics
{

enum class IssueSeverity
{
    // A capability failed while the application remained usable.
    Degraded,
    // Something was asked for and could not be done at all.
    Failed
};

enum class IssueStatus
{
    Open,
    // The capability came back. Kept in the file as history, hidden from the panel.
    Resolved
};

[[nodiscard]] std::string ToString(IssueSeverity value);
[[nodiscard]] std::string ToString(IssueStatus value);
[[nodiscard]] IssueSeverity SeverityFromString(const std::string& value);
[[nodiscard]] IssueStatus StatusFromString(const std::string& value);

struct Issue
{
    // Scope identifies the capability owner; correlation never changes the issue key.
    std::string component;
    std::string code;
    std::string profileId;
    std::string voiceId;
    std::string correlationId;

    IssueSeverity severity = IssueSeverity::Failed;
    IssueStatus status = IssueStatus::Open;

    // One line, in the user's terms. "Voice is using the Windows fallback."
    std::string summary;
    // What actually happened, in the developer's terms.
    std::string detail;
    // Record requires a useful next step.
    std::string remedy;
    // Captured proof: the tail of a worker's stderr, a path, an exit code. Optional,
    // because not every issue has an artifact behind it.
    std::string evidence;

    std::string firstSeen;
    std::string lastSeen;
    int occurrences = 0;

    [[nodiscard]] std::string Key() const;
};

// JSONL issue ledger with coalesced repeats and bounded current-state compaction.
// An interrupted write may leave a trailing partial record without corrupting earlier records.
class IssueLog
{
  public:
    explicit IssueLog(std::filesystem::path path = "Logs/issues.jsonl");

    IssueLog(const IssueLog&) = delete;
    IssueLog& operator=(const IssueLog&) = delete;

    // Retains the in-memory transition even if persistence fails; outError reports it.
    bool Record(const Issue& issue, std::string& outError);

    // Closes an open issue because the capability came back. No-op when nothing is
    // open under that key, so callers can announce success unconditionally.
    bool Resolve(const std::string& component, const std::string& code, const std::string& profileId = {}, const std::string& voiceId = {},
        std::string* outError = nullptr);

    [[nodiscard]] std::vector<Issue> Open() const;
    [[nodiscard]] std::vector<Issue> All() const;
    [[nodiscard]] std::optional<Issue> Find(
        const std::string& component, const std::string& code, const std::string& profileId = {}, const std::string& voiceId = {}) const;
    [[nodiscard]] std::size_t OpenCount() const;
    [[nodiscard]] std::filesystem::path Path() const;

    // Reloads the in-memory view from disk. Used at startup so an issue recorded in a
    // previous session is still visible, and so occurrence counts survive a restart.
    bool Load(std::string& outError);

    // Reads the last lines of a worker's stderr for the evidence field. Returns an
    // empty string when the file cannot be read -- missing evidence must never be the
    // reason an issue goes unrecorded.
    [[nodiscard]] static std::string CaptureTail(const std::filesystem::path& file, std::size_t maximumLines = 20);

    // Bound tracked scopes, evidence and the ordinary failure journal.
    static constexpr std::size_t maximumTrackedIssues = 256;
    static constexpr std::size_t maximumEvidenceBytes = 8192;
    static constexpr std::uintmax_t maximumJournalBytes = 4 * 1024 * 1024;

  private:
    bool Append(const Issue& issue) const;
    bool Compact() const;

    mutable std::mutex mutex;
    std::filesystem::path filePath;
    std::unordered_map<std::string, Issue> issues;
};

} // namespace revia::diagnostics
