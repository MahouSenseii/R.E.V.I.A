#pragma once

// Forward declared so sqlite3 stays out of every caller's translation unit.
struct sqlite3;

#include "Memory/memoryScope.h"
#include <functional>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace revia::memory
{

struct RecallRequest;

struct ArchivedTurn
{
    std::string id;
    std::string sessionId;
    std::string role;
    std::string content;
    // Epoch seconds, as text, matching the durable memory store.
    std::string createdAt;
    int turnIndex = 0;
    MemoryScope scope{};
};

// The text embedded at indexing time and supplied to an optional final reranker.
[[nodiscard]] std::string ArchiveEmbeddingText(const ArchivedTurn& turn);

struct ArchiveEmbeddingBackfillPage
{
    std::vector<ArchivedTurn> entries;
    std::int64_t nextRowId = 0;
    bool hasMore = false;
    std::string error;
};

struct ArchivedSession
{
    std::string id;
    std::string startedAt;
    std::string endedAt;
    std::size_t turns = 0;
    // The first thing the user said, which is a better handle than a timestamp.
    std::string opening;
    // A name the user gave the conversation. Empty until renamed; callers fall back to
    // the opening line.
    std::string title;
};

// What the archive refused, kept so the cost of the filter is visible rather than
// inferred. A user who sees "3 turns withheld" knows the archive is not a transcript.
struct ArchiveCounters
{
    std::size_t recorded = 0;
    std::size_t withheldSensitive = 0;
    std::size_t truncated = 0;
    std::size_t prunedSessions = 0;
};

// Retention is a ceiling, not a target. An archive that grows without bound becomes a
// liability the user never agreed to keep.
struct ArchiveLimits
{
    std::size_t maxSessions = 200;
    std::size_t maxTurnsPerSession = 500;
    std::size_t maxContentCharacters = 8000;
};

// Separate bounded transcript database with its own counters and forget path.
// The shared sensitive-content gate runs here before durable writes.
class ConversationArchive
{
public:
    explicit ConversationArchive(std::string path = "Memory/revia_conversations.db", ArchiveLimits limits = {});
    ~ConversationArchive();

    ConversationArchive(ConversationArchive&&) noexcept;
    ConversationArchive& operator=(ConversationArchive&&) noexcept;

    [[nodiscard]] bool IsAvailable() const;

    // Opens a session and prunes anything past the retention ceiling.
    bool BeginSession(const std::string& sessionId, std::string& outError);
    // Returns false when the turn was withheld or could not be written; outReason says
    // which, so a caller can report a refusal without inventing a cause.
    bool Record(const std::string& sessionId, const std::string& role, const std::string& content, std::string& outReason,
        const MemoryScope& scope = {}, std::function<bool()> admission = {});
    bool EndSession(const std::string& sessionId);

    [[nodiscard]] std::vector<ArchivedTurn> LoadSession(const std::string& sessionId, std::size_t maxTurns = 200) const;
    // The tail of one named session, narrowed to the attributed private scope. Used to
    // continue an older chat; an unattributed or shared scope gets nothing back.
    [[nodiscard]] std::vector<ArchivedTurn> LoadSessionTail(
        const std::string& sessionId, std::size_t maxTurns, const MemoryScope& scope) const;
    [[nodiscard]] bool HasSession(const std::string& sessionId) const;
    // Titles are bounded, valid UTF-8 and pass the same sensitive-content gate as turns.
    bool RenameSession(const std::string& sessionId, const std::string& title, std::string& outError);
    // Removes the newest turn of a session only when it is exactly this role and the
    // stored form of this content. Regenerating a reply retracts what it replaces; a
    // turn the archive withheld or that something else followed is never touched.
    bool RemoveLastTurnIf(const std::string& sessionId, const std::string& role, const std::string& content);
    // The tail of the most recent session that is not this one, for restoring continuity
    // across a restart.
    [[nodiscard]] std::vector<ArchivedTurn> LoadPreviousSessionTail(
        const std::string& currentSessionId, std::size_t maxTurns = 6, const MemoryScope* scope = nullptr) const;
    [[nodiscard]] std::vector<ArchivedTurn> LoadLatestCompatibleTail(
        const std::string& currentSessionId, std::size_t maxTurns, const MemoryScope& scope) const;
    [[nodiscard]] std::vector<ArchivedSession> RecentSessions(std::size_t maxSessions = 20) const;
    // Exact-phrase search, which is what a user typing words into /history means.
    [[nodiscard]] std::vector<ArchivedTurn> Search(const std::string& query, std::size_t maxTurns = 12) const;

    // Everything said in a half-open window of epoch seconds, oldest first, so a stretch
    // of conversation reads in the order it happened. Answers "what did we talk about
    // last Tuesday" without paging whole sessions to find the day.
    [[nodiscard]] std::vector<ArchivedTurn> LoadRange(
        std::int64_t startEpoch, std::int64_t endEpoch, std::size_t maxTurns = 40, const MemoryScope* scope = nullptr) const;

    // The same window narrowed to turns matching any of the supplied terms, best match
    // first. Terms are matched individually rather than as a phrase, because a caller
    // that already reduced a question to its topic words has no phrase left to match.
    [[nodiscard]] std::vector<ArchivedTurn> SearchRange(const std::vector<std::string>& terms, std::int64_t startEpoch,
        std::int64_t endEpoch, std::size_t maxTurns = 12, const MemoryScope* scope = nullptr) const;

    // The earliest turns mentioning any of the terms, oldest first. This is the only
    // honest way to answer "when did I first mention this": relevance ranking returns
    // the best match, which is rarely the first one.
    [[nodiscard]] std::vector<ArchivedTurn> SearchEarliest(
        const std::vector<std::string>& terms, std::size_t maxTurns = 4, const MemoryScope* scope = nullptr) const;

    // Automatic recall still requires an admitted, attributed private request. Both
    // ranked lists are narrowed to its scope and time window before candidate limits.
    // Empty-topic windows and earliest-mention requests preserve chronological order.
    [[nodiscard]] std::vector<ArchivedTurn> SearchHybrid(const RecallRequest& request, std::size_t maxTurns,
        const MemoryScope& scope, const std::vector<float>& queryEmbedding = {},
        const std::string& embeddingModel = "", std::int64_t nowEpoch = 0) const;

    [[nodiscard]] ArchiveEmbeddingBackfillPage ScanMissingEmbeddings(const std::string& embeddingModel,
        std::int64_t afterRowId, std::size_t maxEntries = 25, const MemoryScope* scope = nullptr) const;
    [[nodiscard]] bool NeedsEmbedding(const std::string& turnId, const std::string& embeddingModel) const;
    // A turn ID may be reused after regeneration. Match the captured text and scope
    // as well, so a late vector cannot be attached to its replacement.
    bool SaveEmbedding(const ArchivedTurn& expectedTurn, const std::string& embeddingModel,
        const std::vector<float>& embedding, std::function<bool()> admission = {});

    // Returns how many turns were removed. Forgetting is immediate and total; there is no
    // archived copy kept behind it, because a forget that leaves a copy is not one.
    std::size_t Forget();
    std::size_t ForgetSession(const std::string& sessionId);

    [[nodiscard]] ArchiveCounters Counters() const;
    [[nodiscard]] std::size_t TotalTurns() const;
    [[nodiscard]] std::string Status() const;

private:
    struct Connection;
    // One connection, opened on first use and kept. Two turns are archived per exchange
    // and each open re-ran the schema before it could insert anything, which measured at
    // ten milliseconds a call on a database of a few hundred rows.
    [[nodiscard]] sqlite3* Acquire() const;
    [[nodiscard]] std::vector<ArchivedTurn> LoadCompatibleTail(
        const std::string& currentSessionId, std::size_t maxTurns, const MemoryScope* scope, bool includeCurrent) const;

    std::string archivePath;
    ArchiveLimits limits;
    ArchiveCounters counters;
    mutable std::mutex connectionMutex;
    mutable std::shared_ptr<Connection> connection;
};

// Plain exports of one archived conversation for a file the user chose. Both carry only
// what the archive kept: withheld turns are absent, and the text says so.
[[nodiscard]] std::string RenderTranscriptMarkdown(
    const ArchivedSession& session, const std::vector<ArchivedTurn>& turns, const std::string& assistantName);
[[nodiscard]] std::string RenderTranscriptJson(
    const ArchivedSession& session, const std::vector<ArchivedTurn>& turns, const std::string& assistantName);

} // namespace revia::memory
