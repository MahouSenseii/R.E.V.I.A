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
    // A capability was lost but Revia kept running on a lesser path: Qwen voice fell
    // back to Windows SAPI, embeddings fell back to SQLite FTS. These are the ones
    // that hurt, because nothing looks broken -- it just quietly does less.
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
    // Identity is component + code, never the message. A message carries a timestamp,
    // a path, a byte count -- format it into the identity and the same fault becomes a
    // hundred different issues.
    std::string component;
    std::string code;

    IssueSeverity severity = IssueSeverity::Failed;
    IssueStatus status = IssueStatus::Open;

    // One line, in the user's terms. "Voice is using the Windows fallback."
    std::string summary;
    // What actually happened, in the developer's terms.
    std::string detail;
    // What to do about it. An issue without a remedy is a log line wearing a costume;
    // Record rejects an empty one, because the whole reason this exists is that
    // Revia's logs said what broke and never what to do.
    std::string remedy;
    // Captured proof: the tail of a worker's stderr, a path, an exit code. Optional,
    // because not every issue has an artifact behind it.
    std::string evidence;

    std::string firstSeen;
    std::string lastSeen;
    int occurrences = 0;

    [[nodiscard]] std::string Key() const { return component + "/" + code; }
};

// Append-only JSONL issue ledger with coalesced repeats, resolution, remedy and evidence.
// An interrupted write may leave a trailing partial record without corrupting earlier records.
class IssueLog
{
public:
    explicit IssueLog(std::filesystem::path path = "Logs/issues.jsonl");

    IssueLog(const IssueLog&) = delete;
    IssueLog& operator=(const IssueLog&) = delete;

    // Records a new issue or folds an occurrence into the existing one. Returns false
    // and sets outError when the record is unusable -- an empty component, code, or
    // remedy. Never throws: this runs on failure paths, where a diagnostic that can
    // itself fail loudly is worse than the fault it reports.
    bool Record(const Issue& issue, std::string& outError);

    // Closes an open issue because the capability came back. No-op when nothing is
    // open under that key, so callers can announce success unconditionally.
    bool Resolve(const std::string& component, const std::string& code);

    [[nodiscard]] std::vector<Issue> Open() const;
    [[nodiscard]] std::vector<Issue> All() const;
    [[nodiscard]] std::optional<Issue> Find(const std::string& component, const std::string& code) const;
    [[nodiscard]] std::size_t OpenCount() const;
    [[nodiscard]] std::filesystem::path Path() const;

    // Reloads the in-memory view from disk. Used at startup so an issue recorded in a
    // previous session is still visible, and so occurrence counts survive a restart.
    bool Load(std::string& outError);

    // Reads the last lines of a worker's stderr for the evidence field. Returns an
    // empty string when the file cannot be read -- missing evidence must never be the
    // reason an issue goes unrecorded.
    [[nodiscard]] static std::string CaptureTail(const std::filesystem::path& file, std::size_t maximumLines = 20);

    // Bounds. Both exist because this writes on failure paths, and a fault that fires
    // in a loop must not be able to fill a disk.
    static constexpr std::size_t maximumTrackedIssues = 256;
    static constexpr std::size_t maximumEvidenceBytes = 8192;

private:
    bool Append(const Issue& issue) const;

    mutable std::mutex mutex;
    std::filesystem::path filePath;
    std::unordered_map<std::string, Issue> issues;
};

} // namespace revia::diagnostics
