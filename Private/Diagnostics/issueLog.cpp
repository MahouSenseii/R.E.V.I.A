#include "Diagnostics/issueLog.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::diagnostics
{

namespace
{
std::string Timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif
    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

std::string StringValue(const nlohmann::json& value, const char* key)
{
    return value.contains(key) && value[key].is_string() ? value[key].get<std::string>() : std::string();
}

// Evidence is worker output, so it can be enormous -- a progress bar alone can emit
// megabytes. Truncate from the FRONT: the end of a failing process's stderr is the part
// that explains the failure.
std::string ClampEvidence(std::string value)
{
    if (value.size() <= IssueLog::maximumEvidenceBytes)
    {
        return value;
    }
    const std::size_t start = value.size() - IssueLog::maximumEvidenceBytes;
    return "...(truncated)...\n" + value.substr(start);
}

nlohmann::json IssueRecord(const Issue& issue)
{
    return {{"component", issue.component}, {"code", issue.code}, {"profileId", issue.profileId}, {"voiceId", issue.voiceId},
        {"correlationId", issue.correlationId}, {"severity", ToString(issue.severity)}, {"status", ToString(issue.status)},
        {"summary", issue.summary}, {"detail", issue.detail}, {"remedy", issue.remedy}, {"evidence", issue.evidence},
        {"firstSeen", issue.firstSeen}, {"lastSeen", issue.lastSeen}, {"occurrences", issue.occurrences}};
}
}

std::string Issue::Key() const
{
    const std::string legacy = component + "/" + code;
    if (profileId.empty() && voiceId.empty())
        return legacy;
    return legacy + '\x1f' + std::to_string(profileId.size()) + ':' + profileId + std::to_string(voiceId.size()) + ':' + voiceId;
}

std::string ToString(const IssueSeverity value)
{
    return value == IssueSeverity::Degraded ? "Degraded" : "Failed";
}

std::string ToString(const IssueStatus value)
{
    return value == IssueStatus::Resolved ? "Resolved" : "Open";
}

IssueSeverity SeverityFromString(const std::string& value)
{
    // Unknown text reads as Failed rather than Degraded. An unreadable record is more
    // likely to be hiding something serious than something harmless, and the cost of
    // over-reporting is a line in a panel.
    return value == "Degraded" ? IssueSeverity::Degraded : IssueSeverity::Failed;
}

IssueStatus StatusFromString(const std::string& value)
{
    // Unknown text stays Open, so a corrupt record cannot silently disappear.
    return value == "Resolved" ? IssueStatus::Resolved : IssueStatus::Open;
}

IssueLog::IssueLog(std::filesystem::path path) : filePath(std::move(path))
{
}

std::filesystem::path IssueLog::Path() const
{
    std::lock_guard lock(mutex);
    return filePath;
}

bool IssueLog::Record(const Issue& issue, std::string& outError)
{
    outError.clear();
    if (issue.component.empty() || issue.code.empty())
    {
        outError = "An issue needs a component and a code to be identified by.";
        return false;
    }
    if (issue.remedy.empty())
    {
        outError = "An issue needs a remedy. Without one this belongs in revia.log.";
        return false;
    }
    if (issue.summary.empty())
    {
        outError = "An issue needs a one-line summary.";
        return false;
    }

    try
    {
        std::lock_guard lock(mutex);
        Issue stored;
        const std::string key = issue.Key();
        const auto existing = issues.find(key);
        if (existing == issues.end() && issues.size() >= maximumTrackedIssues)
        {
            outError = "The issue log is full; the oldest issues must be resolved first.";
            return false;
        }

        if (existing == issues.end())
        {
            stored = issue;
            stored.firstSeen = Timestamp();
            stored.occurrences = 1;
            stored.status = IssueStatus::Open;
        }
        else
        {
            // Fold into the existing record. The newest detail and evidence win because
            // they describe the current state, but firstSeen is preserved -- "started
            // at 17:55 and has happened 40 times since" is the useful shape.
            stored = existing->second;
            stored.severity = issue.severity;
            stored.summary = issue.summary;
            stored.detail = issue.detail;
            stored.remedy = issue.remedy;
            stored.correlationId = issue.correlationId;
            if (!issue.evidence.empty())
            {
                stored.evidence = issue.evidence;
            }
            stored.occurrences += 1;
            // A recurrence reopens a resolved issue; the capability evidently left again.
            stored.status = IssueStatus::Open;
        }
        stored.evidence = ClampEvidence(stored.evidence);
        stored.lastSeen = Timestamp();
        issues[key] = stored;
        // State and journal transitions share one order, including resolution.
        if (!Append(stored))
        {
            outError = "The issue was recorded in memory but its history could not be saved.";
            return false;
        }
        return true;
    }
    catch (const std::exception&)
    {
        outError = "The issue history could not be saved.";
        return false;
    }
}

bool IssueLog::Resolve(
    const std::string& component, const std::string& code, const std::string& profileId, const std::string& voiceId, std::string* outError)
{
    if (outError)
        outError->clear();
    try
    {
        std::lock_guard lock(mutex);
        Issue identity;
        identity.component = component;
        identity.code = code;
        identity.profileId = profileId;
        identity.voiceId = voiceId;
        const auto existing = issues.find(identity.Key());
        if (existing == issues.end() || existing->second.status == IssueStatus::Resolved)
        {
            return false;
        }
        existing->second.status = IssueStatus::Resolved;
        existing->second.lastSeen = Timestamp();
        if (!Append(existing->second))
        {
            if (outError)
                *outError = "The issue was resolved in memory but its history could not be saved.";
            return false;
        }
        return true;
    }
    catch (const std::exception&)
    {
        if (outError)
            *outError = "The issue history could not be saved.";
        return false;
    }
}

std::vector<Issue> IssueLog::Open() const
{
    std::vector<Issue> result;
    {
        std::lock_guard lock(mutex);
        for (const auto& [key, issue] : issues)
        {
            if (issue.status == IssueStatus::Open)
            {
                result.push_back(issue);
            }
        }
    }
    // Failures above degradations, then most recent first, so the panel's top line is
    // the thing most worth acting on.
    std::sort(result.begin(), result.end(),
        [](const Issue& left, const Issue& right)
        {
            if (left.severity != right.severity)
            {
                return left.severity == IssueSeverity::Failed;
            }
            return left.lastSeen > right.lastSeen;
        });
    return result;
}

std::vector<Issue> IssueLog::All() const
{
    std::lock_guard lock(mutex);
    std::vector<Issue> result;
    result.reserve(issues.size());
    for (const auto& [key, issue] : issues)
    {
        result.push_back(issue);
    }
    return result;
}

std::optional<Issue> IssueLog::Find(
    const std::string& component, const std::string& code, const std::string& profileId, const std::string& voiceId) const
{
    std::lock_guard lock(mutex);
    Issue identity;
    identity.component = component;
    identity.code = code;
    identity.profileId = profileId;
    identity.voiceId = voiceId;
    const auto existing = issues.find(identity.Key());
    return existing == issues.end() ? std::nullopt : std::optional<Issue>(existing->second);
}

std::size_t IssueLog::OpenCount() const
{
    std::lock_guard lock(mutex);
    return static_cast<std::size_t>(
        std::count_if(issues.begin(), issues.end(), [](const auto& entry) { return entry.second.status == IssueStatus::Open; }));
}

bool IssueLog::Load(std::string& outError)
{
    outError.clear();
    std::lock_guard lock(mutex);
    issues.clear();

    std::error_code error;
    if (!std::filesystem::exists(filePath, error))
    {
        return true;
    }
    std::ifstream file(filePath);
    if (!file.is_open())
    {
        outError = "Could not open " + filePath.string();
        return false;
    }

    std::size_t skipped = 0;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty())
        {
            continue;
        }
        // One unparseable line must not cost the rest of the ledger. This is exactly
        // the case JSON Lines is chosen for: a truncated final write from a process
        // that died is a single bad line, and every earlier record still loads.
        nlohmann::json value;
        try
        {
            value = nlohmann::json::parse(line);
        }
        catch (const std::exception&)
        {
            ++skipped;
            continue;
        }
        if (!value.is_object())
        {
            ++skipped;
            continue;
        }

        Issue issue;
        issue.component = StringValue(value, "component");
        issue.code = StringValue(value, "code");
        issue.profileId = StringValue(value, "profileId");
        issue.voiceId = StringValue(value, "voiceId");
        issue.correlationId = StringValue(value, "correlationId");
        if (issue.component.empty() || issue.code.empty())
        {
            ++skipped;
            continue;
        }
        issue.severity = SeverityFromString(StringValue(value, "severity"));
        issue.status = StatusFromString(StringValue(value, "status"));
        issue.summary = StringValue(value, "summary");
        issue.detail = StringValue(value, "detail");
        issue.remedy = StringValue(value, "remedy");
        issue.evidence = ClampEvidence(StringValue(value, "evidence"));
        issue.firstSeen = StringValue(value, "firstSeen");
        issue.lastSeen = StringValue(value, "lastSeen");
        issue.occurrences = value.contains("occurrences") && value["occurrences"].is_number_integer() ? value["occurrences"].get<int>() : 1;
        // Later lines for the same key supersede earlier ones; the file is a journal,
        // and the last word about a key is its current state.
        const std::string key = issue.Key();
        if (!issues.contains(key) && issues.size() >= maximumTrackedIssues)
        {
            ++skipped;
            continue;
        }
        issues[key] = issue;
    }

    if (skipped > 0)
    {
        outError = "Skipped " + std::to_string(skipped) + " unreadable record(s) in " + filePath.string();
    }
    return true;
}

std::string IssueLog::CaptureTail(const std::filesystem::path& file, const std::size_t maximumLines)
{
    std::error_code error;
    if (maximumLines == 0 || !std::filesystem::is_regular_file(file, error))
    {
        return {};
    }
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        return {};
    }

    std::deque<std::string> tail;
    std::string line;
    while (std::getline(stream, line))
    {
        // Carriage returns survive Windows worker output and turn one progress bar into
        // an unreadable smear in the panel.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }
        tail.push_back(line);
        if (tail.size() > maximumLines)
        {
            tail.pop_front();
        }
    }

    std::string result;
    for (const std::string& entry : tail)
    {
        result += entry;
        result += '\n';
    }
    return ClampEvidence(result);
}

bool IssueLog::Append(const Issue& issue) const
{
    std::error_code error;
    const std::filesystem::path parent = filePath.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error);
        if (error)
            return false;
    }
    const auto bytes = std::filesystem::file_size(filePath, error);
    if (!error && bytes >= maximumJournalBytes)
        return Compact();
    std::ofstream file(filePath, std::ios::app);
    if (!file.is_open())
    {
        return false;
    }
    // dump() with no indent keeps one record on one line, which is what makes the file
    // greppable and what makes a partial write cost exactly one record.
    file << IssueRecord(issue).dump() << '\n';
    file.flush();
    return file.good();
}

bool IssueLog::Compact() const
{
    // Keep every current scope when bounding the journal, including unresolved faults.
    std::filesystem::path temporary = filePath;
    temporary += ".tmp";
    std::ofstream file(temporary, std::ios::trunc);
    if (!file.is_open())
        return false;
    for (const auto& [key, issue] : issues)
        file << IssueRecord(issue).dump() << '\n';
    file.flush();
    if (!file.good())
        return false;
    file.close();
    if (file.fail())
        return false;
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, filePath, error);
    return !error;
#endif
}

} // namespace revia::diagnostics
