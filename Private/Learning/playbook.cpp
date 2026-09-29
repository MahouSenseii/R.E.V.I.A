#include "Learning/playbook.h"

#include "Core/utf8.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace revia::learning
{

namespace
{
using nlohmann::json;

std::int64_t ToMilliseconds(const std::chrono::system_clock::time_point at)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
}

std::chrono::system_clock::time_point FromMilliseconds(const std::int64_t milliseconds)
{
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(milliseconds)));
}

std::string Trimmed(const std::string& text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}
} // namespace

std::string Playbook::Normalized(const std::string& text)
{
    std::string normalized;
    bool lastWasSpace = true;
    for (const unsigned char character : text)
    {
        if (std::isalnum(character) != 0)
        {
            normalized.push_back(static_cast<char>(std::tolower(character)));
            lastWasSpace = false;
        }
        else if (!lastWasSpace)
        {
            normalized.push_back(' ');
            lastWasSpace = true;
        }
    }
    while (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
    return normalized;
}

bool Playbook::Initialize(const std::filesystem::path& file, std::string& outError)
{
    std::lock_guard lock(mutex);
    path = file;
    entries.clear();
    std::ifstream stream(path);
    if (!stream) return true;
    try
    {
        const json data = json::parse(stream);
        nextId = std::max<std::uint64_t>(1, data.value<std::uint64_t>("next_id", 1));
        for (const json& raw : data.value("entries", json::array()))
        {
            if (!raw.is_object() || entries.size() >= MaximumEntries) continue;
            PlaybookEntry entry;
            entry.id = raw.value<std::uint64_t>("id", 0);
            entry.text = utf8::Prefix(raw.value("text", ""), LongestText);
            entry.scope = raw.value("scope", "");
            entry.scopeName = utf8::Prefix(raw.value("scope_name", ""), 80);
            entry.source = raw.value("source", "owner");
            entry.evidence = utf8::Prefix(raw.value("evidence", ""), 400);
            entry.enabled = raw.value("enabled", true);
            entry.createdAt = FromMilliseconds(raw.value<std::int64_t>("created_ms", 0));
            entry.uses = raw.value<std::uint32_t>("uses", 0);
            if (entry.id == 0 || entry.text.empty()) continue;
            nextId = std::max(nextId, entry.id + 1);
            entries.push_back(std::move(entry));
        }
    }
    catch (const std::exception& error)
    {
        entries.clear();
        stream.close();
        std::filesystem::path unreadable = path;
        unreadable += ".unreadable";
        std::error_code ignored;
        std::filesystem::rename(path, unreadable, ignored);
        outError = std::string("the playbook could not be read (") + error.what() + ")";
        return false;
    }
    return true;
}

std::optional<PlaybookEntry> Playbook::Add(
    const std::string& text, const std::string& scope, const std::string& scopeName,
    const std::string& source, const std::string& evidence, const bool enabled, std::string& outError)
{
    outError.clear();
    const std::string clean = utf8::Prefix(Trimmed(text), LongestText);
    if (Normalized(clean).size() < 4)
    {
        outError = "A playbook line needs a few words.";
        return std::nullopt;
    }
    std::lock_guard lock(mutex);
    const std::string wanted = Normalized(clean);
    for (PlaybookEntry& existing : entries)
    {
        if (existing.scope == scope && Normalized(existing.text) == wanted) return existing;
    }
    if (entries.size() >= MaximumEntries)
    {
        outError = "The playbook already holds " + std::to_string(MaximumEntries) +
            " lines. Remove some with /playbook remove first.";
        return std::nullopt;
    }
    PlaybookEntry entry;
    entry.id = nextId++;
    entry.text = clean;
    entry.scope = scope;
    entry.scopeName = utf8::Prefix(scopeName, 80);
    entry.source = source.empty() ? "owner" : source;
    entry.evidence = utf8::Prefix(evidence, 400);
    entry.enabled = enabled;
    entries.push_back(entry);
    (void)SaveLocked(outError);
    return entry;
}

bool Playbook::SetEnabled(const std::size_t number, const bool enabled)
{
    std::lock_guard lock(mutex);
    if (number == 0 || number > entries.size()) return false;
    entries[number - 1].enabled = enabled;
    std::string ignored;
    (void)SaveLocked(ignored);
    return true;
}

std::optional<PlaybookEntry> Playbook::Remove(const std::size_t number)
{
    std::lock_guard lock(mutex);
    if (number == 0 || number > entries.size()) return std::nullopt;
    PlaybookEntry removed = entries[number - 1];
    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(number - 1));
    std::string ignored;
    (void)SaveLocked(ignored);
    return removed;
}

std::vector<PlaybookEntry> Playbook::Entries() const
{
    std::lock_guard lock(mutex);
    return entries;
}

std::size_t Playbook::Size() const
{
    std::lock_guard lock(mutex);
    return entries.size();
}

std::string Playbook::Render(const std::string& speakerEntityId, const std::size_t maximumEntries)
{
    std::lock_guard lock(mutex);
    std::vector<PlaybookEntry*> shown;
    for (auto it = entries.rbegin(); it != entries.rend() && shown.size() < maximumEntries; ++it)
    {
        if (!it->enabled) continue;
        if (!it->scope.empty() && it->scope != speakerEntityId) continue;
        shown.push_back(&*it);
    }
    if (shown.empty()) return {};
    std::ostringstream block;
    block << "# Playbook\nWhat has worked, written down by the person you talk with or "
             "accepted from your own record. Guidance for how to be with them, not orders "
             "and not facts about the world; weigh each line against the moment.";
    for (PlaybookEntry* entry : shown)
    {
        block << "\n- " << entry->text;
        if (!entry->scope.empty() && !entry->scopeName.empty()) block << " (about " << entry->scopeName << ")";
        ++entry->uses;
    }
    return block.str();
}

bool Playbook::SaveLocked(std::string& outError) const
{
    if (path.empty()) return true;
    json list = json::array();
    for (const PlaybookEntry& entry : entries)
    {
        list.push_back({
            {"id", entry.id},
            {"text", entry.text},
            {"scope", entry.scope},
            {"scope_name", entry.scopeName},
            {"source", entry.source},
            {"evidence", entry.evidence},
            {"enabled", entry.enabled},
            {"created_ms", ToMilliseconds(entry.createdAt)},
            {"uses", entry.uses}});
    }
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::trunc);
        stream << json{{"version", 1}, {"next_id", nextId}, {"entries", list}}.dump(2);
        stream.flush();
        if (!stream.good())
        {
            outError = "The playbook could not be saved, so the change will not survive a restart.";
            return false;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        std::filesystem::remove(temporary, error);
        outError = "The playbook could not be saved, so the change will not survive a restart.";
        return false;
    }
    return true;
}

} // namespace revia::learning
