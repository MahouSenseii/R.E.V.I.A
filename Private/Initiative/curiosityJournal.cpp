#include "Initiative/curiosityJournal.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>

namespace revia::initiative
{

namespace
{
constexpr std::size_t MaximumLoadedRecords = 500;
// The file is allowed to run ahead of what is loaded, so ordinary appends stay a single
// write, but not indefinitely: only the newest MaximumLoadedRecords ever affect
// behaviour, and everything before them is dead weight on disk.
constexpr std::uintmax_t MaximumJournalBytes = 1024 * 1024;
// Appends between size checks. Compaction rewrites the whole file, which is not
// something a conversation turn should ever wait behind.
constexpr std::size_t CompactionInterval = 64;

std::int64_t EpochMilliseconds(const std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

CuriosityRecord ParseRecord(const nlohmann::json& data)
{
    CuriosityRecord record;
    record.topic = data.value("topic", "");
    record.query = data.value("query", "");
    record.outcome = data.value("outcome", "");
    if (data.contains("sources") && data["sources"].is_array())
    {
        for (const auto& source : data["sources"])
        {
            if (source.is_string() && record.sources.size() < 10)
            {
                record.sources.push_back(source.get<std::string>());
            }
        }
    }
    record.occurredAt = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(data.value<std::int64_t>("occurred_at_ms", 0)));
    return record;
}
}

std::string CuriosityJournal::NormalizeTopic(const std::string& topic)
{
    std::string normalized;
    normalized.reserve(topic.size());
    bool space = false;
    for (const unsigned char character : topic)
    {
        if (std::isalnum(character) != 0)
        {
            normalized.push_back(static_cast<char>(std::tolower(character)));
            space = false;
        }
        else if (!normalized.empty() && !space)
        {
            normalized.push_back(' ');
            space = true;
        }
    }
    while (!normalized.empty() && normalized.back() == ' ')
    {
        normalized.pop_back();
    }
    return normalized;
}

bool CuriosityJournal::Initialize(
    const std::filesystem::path& path,
    std::string& outError)
{
    std::lock_guard lock(mutex);
    journalPath = path;
    records.clear();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
    {
        outError = "Could not create the curiosity journal directory: " + error.message();
        return false;
    }
    if (!std::filesystem::exists(path, error))
    {
        outError.clear();
        return true;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        outError = "Could not open the curiosity journal.";
        return false;
    }
    // Read through the file handle rather than the directory entry, for the same reason
    // the compaction trigger does not use std::filesystem::file_size.
    stream.seekg(0, std::ios::end);
    journalBytes = static_cast<std::uintmax_t>(
        std::max<std::streamoff>(0, stream.tellg()));
    stream.seekg(0, std::ios::beg);
    std::string line;
    while (std::getline(stream, line))
    {
        try
        {
            CuriosityRecord record = ParseRecord(nlohmann::json::parse(line));
            if (!NormalizeTopic(record.topic).empty())
            {
                records.push_back(std::move(record));
                if (records.size() > MaximumLoadedRecords)
                {
                    records.erase(records.begin());
                }
            }
        }
        catch (const std::exception&)
        {
            // One interrupted append must not make all earlier topic history unusable.
        }
    }
    outError.clear();
    return true;
}

bool CuriosityJournal::WasRecentlyConsidered(
    const std::string& topic,
    const std::chrono::minutes window,
    const std::chrono::system_clock::time_point now) const
{
    const std::string wanted = NormalizeTopic(topic);
    if (wanted.empty()) return true;
    std::lock_guard lock(mutex);
    return std::any_of(records.rbegin(), records.rend(), [&](const CuriosityRecord& record)
    {
        return NormalizeTopic(record.topic) == wanted &&
            now >= record.occurredAt && now - record.occurredAt <= window;
    });
}

bool CuriosityJournal::WasResearchRecentlyAttempted(
    const std::chrono::seconds window,
    const std::chrono::system_clock::time_point now) const
{
    std::lock_guard lock(mutex);
    return std::any_of(records.rbegin(), records.rend(), [&](const CuriosityRecord& record)
    {
        return !record.query.empty() && now >= record.occurredAt &&
            now - record.occurredAt <= window;
    });
}

bool CuriosityJournal::Append(const CuriosityRecord& input, std::string& outError)
{
    CuriosityRecord record = input;
    if (NormalizeTopic(record.topic).empty())
    {
        outError = "A curiosity record requires a concrete topic.";
        return false;
    }
    record.topic.resize(std::min<std::size_t>(record.topic.size(), 300));
    record.query.resize(std::min<std::size_t>(record.query.size(), 500));
    record.outcome.resize(std::min<std::size_t>(record.outcome.size(), 1000));
    if (record.sources.size() > 10) record.sources.resize(10);

    std::lock_guard lock(mutex);
    if (journalPath.empty())
    {
        outError = "The curiosity journal is not initialized.";
        return false;
    }
    std::ofstream stream(journalPath, std::ios::binary | std::ios::app);
    if (!stream)
    {
        outError = "Could not append to the curiosity journal.";
        return false;
    }
    const std::string line = nlohmann::json({
        {"occurred_at_ms", EpochMilliseconds(record.occurredAt)},
        {"topic", record.topic},
        {"query", record.query},
        {"sources", record.sources},
        {"outcome", record.outcome}
    }).dump();
    stream << line << '\n';
    if (!stream.good())
    {
        outError = "Could not finish writing the curiosity journal.";
        return false;
    }
    journalBytes += line.size() + 1;
    records.push_back(std::move(record));
    if (records.size() > MaximumLoadedRecords) records.erase(records.begin());

    // Only the newest MaximumLoadedRecords ever affect behaviour, so the file has no
    // reason to grow past a small multiple of that. Checked here, acted on rarely: the
    // rewrite happens once every CompactionInterval appends rather than on each one, so
    // conversation never waits behind a full file rewrite.
    stream.close();
    if (++appendsSinceCompaction >= CompactionInterval)
    {
        appendsSinceCompaction = 0;
        std::string compactError;
        if (!CompactUnlocked(compactError))
        {
            // The previous journal is still intact and still correct; compaction is an
            // optimisation, and failing it must not fail the append that triggered it.
            outError.clear();
            return true;
        }
    }
    outError.clear();
    return true;
}

bool CuriosityJournal::CompactUnlocked(std::string& outError)
{
    // Triggered on bytes this process has written, not on std::filesystem::file_size.
    //
    // On Windows that call reads the directory entry, which is not refreshed while a
    // file is being appended to: measured here it reported 620 KB for a journal that
    // was actually 2.1 MB, so compaction never fired at all. The running total is exact
    // and costs nothing.
    if (journalBytes <= MaximumJournalBytes) return true;

    std::error_code error;
    std::uintmax_t rewrittenBytes = 0;
    // Written beside the journal and moved into place, so an interrupted compaction
    // leaves the previous good file untouched rather than a half-written one.
    const std::filesystem::path temporary =
        journalPath.string() + ".compact-" + std::to_string(EpochMilliseconds(
            std::chrono::system_clock::now()));
    {
        std::ofstream rewritten(temporary, std::ios::binary | std::ios::trunc);
        if (!rewritten)
        {
            outError = "The curiosity journal could not be compacted.";
            return false;
        }
        for (const CuriosityRecord& kept : records)
        {
            const std::string keptLine = nlohmann::json({
                {"occurred_at_ms", EpochMilliseconds(kept.occurredAt)},
                {"topic", kept.topic},
                {"query", kept.query},
                {"sources", kept.sources},
                {"outcome", kept.outcome}
            }).dump();
            rewritten << keptLine << '\n';
            rewrittenBytes += keptLine.size() + 1;
        }
        rewritten.flush();
        if (!rewritten.good())
        {
            rewritten.close();
            std::filesystem::remove(temporary, error);
            outError = "The compacted curiosity journal could not be written.";
            return false;
        }
    }
    std::filesystem::rename(temporary, journalPath, error);
    if (error)
    {
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        outError = "The compacted curiosity journal could not replace the original.";
        return false;
    }
    journalBytes = rewrittenBytes;
    return true;
}

std::vector<CuriosityRecord> CuriosityJournal::Recent(const std::size_t maximum) const
{
    std::lock_guard lock(mutex);
    const std::size_t count = std::min(maximum, records.size());
    return std::vector<CuriosityRecord>(records.end() - static_cast<std::ptrdiff_t>(count),
        records.end());
}

} // namespace revia::initiative
