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
    stream << nlohmann::json({
        {"occurred_at_ms", EpochMilliseconds(record.occurredAt)},
        {"topic", record.topic},
        {"query", record.query},
        {"sources", record.sources},
        {"outcome", record.outcome}
    }).dump() << '\n';
    if (!stream.good())
    {
        outError = "Could not finish writing the curiosity journal.";
        return false;
    }
    records.push_back(std::move(record));
    if (records.size() > MaximumLoadedRecords) records.erase(records.begin());
    outError.clear();
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
