#pragma once

// Forward declared so sqlite3 stays out of every caller's translation unit.
struct sqlite3;

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace revia::memory
{

struct ArchivedTurn
{
    std::string id;
    std::string sessionId;
    std::string role;
    std::string content;
    // Epoch seconds, as text, matching the durable memory store.
    std::string createdAt;
    int turnIndex = 0;
};

struct ArchivedSession
{
    std::string id;
    std::string startedAt;
    std::string endedAt;
    std::size_t turns = 0;
    // The first thing the user said, which is a better handle than a timestamp.
    std::string opening;
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

// Durable conversation history: what was actually said, searchable, bounded, forgettable.
//
// Deliberately separate from longTermMemory, which stores curated facts a classifier
// judged worth keeping. This stores the exchange itself, which is a materially larger
// promise -- so it carries its own database file, its own retention ceiling, its own
// counters, and its own forget command, rather than quietly enlarging what "memory"
// already meant.
//
// Content matching the shared sensitive-content markers is never written. That check runs
// here rather than at the call site, because an archive that depends on every caller
// remembering to filter is one forgotten call away from storing a password.
class ConversationArchive
{
public:
    explicit ConversationArchive(
        std::string path = "Memory/revia_conversations.db",
        ArchiveLimits limits = {});
    ~ConversationArchive();

    ConversationArchive(ConversationArchive&&) noexcept;
    ConversationArchive& operator=(ConversationArchive&&) noexcept;

    [[nodiscard]] bool IsAvailable() const;

    // Opens a session and prunes anything past the retention ceiling.
    bool BeginSession(const std::string& sessionId, std::string& outError);
    // Returns false when the turn was withheld or could not be written; outReason says
    // which, so a caller can report a refusal without inventing a cause.
    bool Record(
        const std::string& sessionId,
        const std::string& role,
        const std::string& content,
        std::string& outReason);
    bool EndSession(const std::string& sessionId);

    [[nodiscard]] std::vector<ArchivedTurn> LoadSession(
        const std::string& sessionId,
        std::size_t maxTurns = 200) const;
    // The tail of the most recent session that is not this one, for restoring continuity
    // across a restart.
    [[nodiscard]] std::vector<ArchivedTurn> LoadPreviousSessionTail(
        const std::string& currentSessionId,
        std::size_t maxTurns = 6) const;
    [[nodiscard]] std::vector<ArchivedSession> RecentSessions(
        std::size_t maxSessions = 20) const;
    [[nodiscard]] std::vector<ArchivedTurn> Search(
        const std::string& query,
        std::size_t maxTurns = 12) const;

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

    std::string archivePath;
    ArchiveLimits limits;
    ArchiveCounters counters;
    mutable std::mutex connectionMutex;
    mutable std::shared_ptr<Connection> connection;
};

} // namespace revia::memory
