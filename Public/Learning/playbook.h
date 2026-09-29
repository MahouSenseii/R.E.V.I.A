#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::learning
{

// What works, written down where she can read it and the owner can edit it.
//
// The first rung of learning that never touches weights. A playbook line is one
// sentence of guidance -- "keep answers short in the evening", "Sam wants the
// reasoning before the answer" -- scoped to everyone or to one person, entered by the
// owner in their own words or accepted from a lesson she drew from her own record.
// Enabled lines enter her prompt for the turns they apply to, marked as guidance and
// never as orders; a line can be switched off or removed at any time, and nothing
// here can widen a capability, change a budget or alter how an action is authorised.
struct PlaybookEntry
{
    std::uint64_t id = 0;
    std::string text;
    // The person it is about, as an entity id; empty means everyone.
    std::string scope;
    // Their name as it was given, for the list.
    std::string scopeName;
    // "owner", "lesson" or "review".
    std::string source;
    std::string evidence;
    bool enabled = true;
    std::chrono::system_clock::time_point createdAt = std::chrono::system_clock::now();
    // Turns it was shown on.
    std::uint32_t uses = 0;
};

class Playbook
{
public:
    static constexpr std::size_t MaximumEntries = 200;
    static constexpr std::size_t LongestText = 240;
    static constexpr std::size_t DefaultShown = 12;

    // Loads what was saved. Without it the book still works, in memory only.
    bool Initialize(const std::filesystem::path& path, std::string& outError);

    // Adds a line, or returns the existing one when the same words are already there
    // for the same scope. Empty with the reason when the book is full or the text is
    // unusable.
    std::optional<PlaybookEntry> Add(
        const std::string& text, const std::string& scope, const std::string& scopeName,
        const std::string& source, const std::string& evidence, bool enabled, std::string& outError);
    // `number` counts from 1 in Entries() order.
    bool SetEnabled(std::size_t number, bool enabled);
    std::optional<PlaybookEntry> Remove(std::size_t number);
    [[nodiscard]] std::vector<PlaybookEntry> Entries() const;
    [[nodiscard]] std::size_t Size() const;

    // The block for one turn: enabled lines for everyone and for `speakerEntityId`,
    // newest first, at most `maximumEntries`. Empty when there is nothing to say.
    // Counts a use on each line shown.
    [[nodiscard]] std::string Render(const std::string& speakerEntityId, std::size_t maximumEntries = DefaultShown);

    // Case- and punctuation-insensitive form, for spotting the same line twice.
    [[nodiscard]] static std::string Normalized(const std::string& text);

private:
    bool SaveLocked(std::string& outError) const;

    mutable std::mutex mutex;
    std::filesystem::path path;
    std::vector<PlaybookEntry> entries;
    std::uint64_t nextId = 1;
};

} // namespace revia::learning
