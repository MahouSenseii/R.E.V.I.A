#pragma once

#include "Library/structLibrary.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// The live conversation: recent messages word for word, and what came before them in
// compact form.
//
// Thread-safe. Turns add to it on the conversation worker while the history compactor
// reads and folds its oldest part on a background thread.
class conversationContext
{
public:
    conversationContext();
    ~conversationContext();

    void AddMessage(const std::string& role, const std::string& content);
    // Rolls back only the exact newest message. Used when fresh user input cancels an
    // autonomous result during its final commit race; older dialogue is never searched
    // or removed by content.
    [[nodiscard]] bool RemoveLastMessageIf(
        const std::string& role,
        const std::string& content);
    void Clear();

    std::vector<conversationMessage> GetRecentMessages() const;
    [[nodiscard]] std::string GetCompressedHistorySummary() const;

    // One piece of compaction work: the oldest messages, and whatever was already cut
    // short on its way out, to be folded into the running summary.
    struct CompactionJob
    {
        std::string previousSummary;
        // Messages evicted before they could be summarised, as the one-line excerpts
        // they were cut down to. Older than every message below.
        std::string evictedExcerpts;
        std::vector<conversationMessage> messages;
        // What the result will replace, and the state it was taken from. A job whose
        // history was cleared, or whose summary was replaced, is refused on apply.
        std::uint64_t throughSequence = 0;
        std::uint64_t generation = 0;
        std::uint64_t summaryVersion = 0;
        // Messages left word for word once this is applied.
        std::size_t keptVerbatim = 0;
    };

    // Whether the kept conversation has reached three quarters of its budget, or
    // something was already cut short because compaction had not caught up.
    [[nodiscard]] bool NeedsCompaction() const;
    // The oldest half of the kept conversation, ending on a complete exchange, and never
    // the newest few messages. Empty when there is nothing worth folding.
    [[nodiscard]] std::optional<CompactionJob> BeginCompaction() const;
    // Replaces everything `job` covered with `summary`. Messages added since the job was
    // taken are untouched. Returns false, and changes nothing, when the job is stale or
    // the summary is empty.
    bool ApplyCompaction(const CompactionJob& job, std::string summary);
    // A summary carried over from an earlier session. It is older than every message
    // already here.
    void RestoreSummary(std::string summary);
    // The model-written summary alone, without excerpts or framing. For persistence.
    [[nodiscard]] std::string Summary() const;

    // Enough to hold what later turns depend on, small enough to ride along every turn.
    static constexpr std::size_t MaximumSummaryCharacters = 1600;
    // Never folded, however full the history: the exchange being continued has to stay
    // in her own words.
    static constexpr std::size_t MinimumVerbatimMessages = 4;

private:
    struct Retained
    {
        conversationMessage message;
        std::uint64_t sequence = 0;
    };
    struct Excerpt
    {
        std::string line;
        std::uint64_t sequence = 0;
    };

    // All of these expect `mutex` to be held.
    [[nodiscard]] std::size_t CharacterCount() const;
    [[nodiscard]] std::size_t MaximumRetainedMessageCharacters() const;
    [[nodiscard]] bool NeedsCompactionLocked() const;
    [[nodiscard]] std::string ExcerptText() const;
    void BoundRetainedMessage(conversationMessage& message) const;
    void TrimToBudget();
    void CompressOldMessage(const Retained& message);

    mutable std::mutex mutex;
    std::vector<Retained> messages;
    std::deque<Excerpt> excerpts;
    std::string summary;
    std::uint64_t nextSequence = 1;
    std::uint64_t generation = 0;
    std::uint64_t summaryVersion = 0;
    std::size_t maxMessages = 24;
    std::size_t maxCharacters = 14000;
    std::size_t maxSummaryCharacters = 2400;
};
