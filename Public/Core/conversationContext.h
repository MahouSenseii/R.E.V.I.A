#pragma once

#include "Library/structLibrary.h"
#include "Memory/observationLog.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// The live conversation: recent messages word for word, and what came before them as a
// log of dated observations.
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
    // short on its way out, to be turned into observations for the log.
    struct CompactionJob
    {
        // What the log already holds, rendered, so the observer adds to it rather than
        // repeating it.
        std::string knownObservations;
        // Messages evicted before they could be observed, as the one-line excerpts
        // they were cut down to. Older than every message below.
        std::string evictedExcerpts;
        std::vector<conversationMessage> messages;
        // The sequence range the result will stand for, and the state it was taken
        // from. A job whose history was cleared, or whose log was changed by a
        // reflection, is refused on apply.
        std::uint64_t fromSequence = 0;
        std::uint64_t throughSequence = 0;
        std::uint64_t generation = 0;
        std::uint64_t logVersion = 0;
        // Messages left word for word once this is applied.
        std::size_t keptVerbatim = 0;
    };

    // Whether the kept conversation has reached three quarters of its budget, or
    // something was already cut short because compaction had not caught up.
    [[nodiscard]] bool NeedsCompaction() const;
    // The oldest half of the kept conversation, ending on a complete exchange, and never
    // the newest few messages. Empty when there is nothing worth folding.
    [[nodiscard]] std::optional<CompactionJob> BeginCompaction() const;
    // Appends `observations` to the log, dated now and sourced to the job's range, and
    // drops everything the job covered. Messages added since the job was taken are
    // untouched. Returns the ids assigned, or nothing -- and changes nothing -- when the
    // job is stale or no observation survived bounding.
    [[nodiscard]] std::vector<std::uint64_t> ApplyCompaction(
        const CompactionJob& job,
        std::vector<revia::memory::Observation> observations);

    // One reflection: the current observations, to be merged where they overlap.
    struct ReflectionJob
    {
        std::vector<revia::memory::Observation> observations;
        std::uint64_t generation = 0;
        std::uint64_t logVersion = 0;
    };
    struct Merge
    {
        std::vector<std::uint64_t> replaces;
        revia::memory::Observation merged;
    };
    [[nodiscard]] bool NeedsReflection() const;
    [[nodiscard]] std::optional<ReflectionJob> BeginReflection() const;
    // Applies each merge that still names current observations; the rest are skipped.
    // Returns the ids of the merged observations, in order, paired with what each one
    // replaced. Empty, with nothing changed, when the job is stale.
    [[nodiscard]] std::vector<Merge> ApplyReflection(
        const ReflectionJob& job,
        const std::vector<Merge>& merges);

    // Observations carried over from an earlier session. Older than every message
    // already here.
    void RestoreObservations(std::vector<revia::memory::Observation> observations);
    // The log itself, for persistence and the panel.
    [[nodiscard]] std::vector<revia::memory::Observation> Observations() const;
    // The log rendered for the prompt, framed as a record, or empty. Changes only when
    // the log does, which is what lets it sit in the stable part of the prompt.
    [[nodiscard]] std::string RenderObservations() const;
    // Excerpts cut short by eviction before the observer reached them, framed, or
    // empty. Changes whenever something is evicted, so it belongs with per-turn text.
    [[nodiscard]] std::string RenderExcerpts() const;

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
    revia::memory::ObservationLog log;
    std::uint64_t nextSequence = 1;
    std::uint64_t generation = 0;
    std::uint64_t logVersion = 0;
    std::size_t maxMessages = 24;
    std::size_t maxCharacters = 14000;
    std::size_t maxSummaryCharacters = 2400;
};
