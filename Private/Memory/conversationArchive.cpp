#include "Memory/conversationArchive.h"

#include "Memory/sensitiveContent.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <memory>
#include <sqlite3.h>
#include <sstream>
#include <system_error>
#include <utility>

namespace revia::memory
{

namespace
{

struct DatabaseCloser
{
    void operator()(sqlite3* database) const
    {
        if (database != nullptr)
        {
            sqlite3_close(database);
        }
    }
};

struct StatementCloser
{
    void operator()(sqlite3_stmt* statement) const
    {
        if (statement != nullptr)
        {
            sqlite3_finalize(statement);
        }
    }
};

using Database = std::unique_ptr<sqlite3, DatabaseCloser>;
using Statement = std::unique_ptr<sqlite3_stmt, StatementCloser>;

bool Execute(sqlite3* database, const char* sql)
{
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    if (error != nullptr)
    {
        sqlite3_free(error);
    }
    return result == SQLITE_OK;
}

std::string CurrentEpochSeconds()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

std::string ColumnText(sqlite3_stmt* statement, const int column)
{
    const unsigned char* value = sqlite3_column_text(statement, column);
    return value == nullptr
        ? std::string()
        : std::string(reinterpret_cast<const char*>(value));
}

Database OpenDatabase(const std::string& archivePath)
{
    const std::filesystem::path path(archivePath);
    std::error_code directoryError;
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path(), directoryError);
        if (directoryError)
        {
            return {};
        }
    }

    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(
            archivePath.c_str(),
            &raw,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK)
    {
        if (raw != nullptr)
        {
            sqlite3_close(raw);
        }
        return {};
    }

    Database database(raw);
    sqlite3_busy_timeout(database.get(), 2000);
    constexpr const char* Schema =
        "PRAGMA journal_mode=WAL;"
        // NORMAL, not the default FULL. Under a write-ahead log this still survives a
        // process crash and only risks the newest commit on power loss, and it removes
        // an fsync from every archived turn.
        "PRAGMA synchronous=NORMAL;"
        "CREATE TABLE IF NOT EXISTS conversation_sessions ("
        "  session_id TEXT PRIMARY KEY,"
        "  started_at TEXT NOT NULL,"
        "  ended_at TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS conversation_turns ("
        "  id TEXT NOT NULL UNIQUE,"
        "  session_id TEXT NOT NULL,"
        "  turn_index INTEGER NOT NULL,"
        "  role TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS conversation_turns_session "
        "  ON conversation_turns(session_id, turn_index);"
        "CREATE VIRTUAL TABLE IF NOT EXISTS conversation_search USING fts5("
        "  content, content='conversation_turns', content_rowid='rowid',"
        "  tokenize='unicode61 remove_diacritics 2'"
        ");"
        "CREATE TRIGGER IF NOT EXISTS conversation_turns_after_insert "
        "  AFTER INSERT ON conversation_turns BEGIN "
        "  INSERT INTO conversation_search(rowid, content) VALUES (new.rowid, new.content);"
        "END;"
        "CREATE TRIGGER IF NOT EXISTS conversation_turns_after_delete "
        "  AFTER DELETE ON conversation_turns BEGIN "
        "  INSERT INTO conversation_search(conversation_search, rowid, content) "
        "  VALUES ('delete', old.rowid, old.content);"
        "END;";
    if (!Execute(database.get(), Schema))
    {
        return {};
    }
    return database;
}

// FTS5 treats a bare apostrophe or quote as syntax. A user searching for what they said
// should never see a query error, so the phrase is quoted and internal quotes doubled.
std::string QuoteForFts(const std::string& query)
{
    std::string quoted = "\"";
    for (const char character : query)
    {
        if (character == '"')
        {
            quoted += "\"\"";
            continue;
        }
        quoted.push_back(character);
    }
    quoted.push_back('"');
    return quoted;
}

ArchivedTurn ReadTurn(sqlite3_stmt* statement)
{
    ArchivedTurn turn;
    turn.id = ColumnText(statement, 0);
    turn.sessionId = ColumnText(statement, 1);
    turn.turnIndex = sqlite3_column_int(statement, 2);
    turn.role = ColumnText(statement, 3);
    turn.content = ColumnText(statement, 4);
    turn.createdAt = ColumnText(statement, 5);
    return turn;
}

constexpr const char* TurnColumns =
    "id, session_id, turn_index, role, content, created_at";

} // namespace

struct ConversationArchive::Connection
{
    Database database;
};

ConversationArchive::ConversationArchive(std::string path, ArchiveLimits inputLimits)
    : archivePath(std::move(path)), limits(inputLimits)
{
}

ConversationArchive::~ConversationArchive() = default;

ConversationArchive::ConversationArchive(ConversationArchive&& other) noexcept
    : archivePath(std::move(other.archivePath)),
      limits(other.limits),
      counters(other.counters)
{
    std::lock_guard lock(other.connectionMutex);
    connection = std::move(other.connection);
}

ConversationArchive& ConversationArchive::operator=(ConversationArchive&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    // Both mutexes, ordered by address, so two archives moved past each other on
    // different threads cannot deadlock against one another.
    std::scoped_lock lock(connectionMutex, other.connectionMutex);
    archivePath = std::move(other.archivePath);
    limits = other.limits;
    counters = other.counters;
    connection = std::move(other.connection);
    return *this;
}

sqlite3* ConversationArchive::Acquire() const
{
    std::lock_guard lock(connectionMutex);
    if (connection && connection->database)
    {
        return connection->database.get();
    }
    Database opened = OpenDatabase(archivePath);
    if (opened == nullptr)
    {
        return nullptr;
    }
    connection = std::make_shared<Connection>();
    connection->database = std::move(opened);
    return connection->database.get();
}

bool ConversationArchive::IsAvailable() const
{
    return Acquire() != nullptr;
}

bool ConversationArchive::BeginSession(const std::string& sessionId, std::string& outError)
{
    sqlite3* const database = Acquire();
    if (database == nullptr)
    {
        outError = "The conversation archive could not be opened.";
        return false;
    }

    {
        Statement statement;
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(database,
                "INSERT OR IGNORE INTO conversation_sessions(session_id, started_at) "
                "VALUES (?, ?);",
                -1, &raw, nullptr) != SQLITE_OK)
        {
            outError = "The conversation archive rejected a new session.";
            return false;
        }
        statement.reset(raw);
        sqlite3_bind_text(statement.get(), 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
        const std::string now = CurrentEpochSeconds();
        sqlite3_bind_text(statement.get(), 2, now.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(statement.get()) != SQLITE_DONE)
        {
            outError = "The conversation archive could not record the session start.";
            return false;
        }
    }

    // Pruned on open rather than on write. Retention is about what the archive is allowed
    // to hold over time, and checking it once per session costs nothing per turn.
    Statement statement;
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database,
            "SELECT session_id FROM conversation_sessions "
            "ORDER BY CAST(started_at AS INTEGER) DESC LIMIT -1 OFFSET ?;",
            -1, &raw, nullptr) != SQLITE_OK)
    {
        return true;
    }
    statement.reset(raw);
    sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(limits.maxSessions));
    std::vector<std::string> expired;
    while (sqlite3_step(statement.get()) == SQLITE_ROW)
    {
        expired.push_back(ColumnText(statement.get(), 0));
    }
    statement.reset();

    for (const std::string& id : expired)
    {
        ForgetSession(id);
        ++counters.prunedSessions;
    }
    return true;
}

bool ConversationArchive::Record(
    const std::string& sessionId,
    const std::string& role,
    const std::string& content,
    std::string& outReason)
{
    if (content.empty() || role.empty() || sessionId.empty())
    {
        outReason = "Nothing to record.";
        return false;
    }
    if (ContainsSensitiveContent(content))
    {
        // Withheld rather than redacted. A redacted turn still says how long the secret
        // was and where it sat in the sentence, and the archive has no need for either.
        ++counters.withheldSensitive;
        outReason = "The turn matched a sensitive-content marker and was not archived.";
        return false;
    }

    std::string stored = content;
    if (stored.size() > limits.maxContentCharacters)
    {
        stored.resize(limits.maxContentCharacters);
        stored += " [truncated]";
        ++counters.truncated;
    }

    sqlite3* const database = Acquire();
    if (database == nullptr)
    {
        outReason = "The conversation archive could not be opened.";
        return false;
    }

    int nextIndex = 0;
    {
        Statement statement;
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(database,
                "SELECT COALESCE(MAX(turn_index), -1) + 1 FROM conversation_turns "
                "WHERE session_id = ?;",
                -1, &raw, nullptr) == SQLITE_OK)
        {
            statement.reset(raw);
            sqlite3_bind_text(statement.get(), 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(statement.get()) == SQLITE_ROW)
            {
                nextIndex = sqlite3_column_int(statement.get(), 0);
            }
        }
    }

    if (static_cast<std::size_t>(nextIndex) >= limits.maxTurnsPerSession)
    {
        outReason = "This session has reached its archive ceiling.";
        return false;
    }

    Statement statement;
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database,
            "INSERT INTO conversation_turns"
            "(id, session_id, turn_index, role, content, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?);",
            -1, &raw, nullptr) != SQLITE_OK)
    {
        outReason = "The conversation archive rejected the turn.";
        return false;
    }
    statement.reset(raw);
    const std::string createdAt = CurrentEpochSeconds();
    const std::string id = sessionId + ":" + std::to_string(nextIndex);
    sqlite3_bind_text(statement.get(), 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, sessionId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement.get(), 3, nextIndex);
    sqlite3_bind_text(statement.get(), 4, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 5, stored.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 6, createdAt.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement.get()) != SQLITE_DONE)
    {
        outReason = "The conversation archive could not store the turn.";
        return false;
    }
    ++counters.recorded;
    return true;
}

bool ConversationArchive::EndSession(const std::string& sessionId)
{
    sqlite3* const database = Acquire();
    if (database == nullptr)
    {
        return false;
    }
    Statement statement;
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database,
            "UPDATE conversation_sessions SET ended_at = ? WHERE session_id = ?;",
            -1, &raw, nullptr) != SQLITE_OK)
    {
        return false;
    }
    statement.reset(raw);
    const std::string now = CurrentEpochSeconds();
    sqlite3_bind_text(statement.get(), 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, sessionId.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(statement.get()) == SQLITE_DONE;
}

std::vector<ArchivedTurn> ConversationArchive::LoadSession(
    const std::string& sessionId,
    const std::size_t maxTurns) const
{
    std::vector<ArchivedTurn> turns;
    sqlite3* const database = Acquire();
    if (database == nullptr)
    {
        return turns;
    }
    Statement statement;
    sqlite3_stmt* raw = nullptr;
    const std::string sql = std::string("SELECT ") + TurnColumns +
        " FROM conversation_turns WHERE session_id = ? ORDER BY turn_index ASC LIMIT ?;";
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
    {
        return turns;
    }
    statement.reset(raw);
    sqlite3_bind_text(statement.get(), 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(maxTurns));
    while (sqlite3_step(statement.get()) == SQLITE_ROW)
    {
        turns.push_back(ReadTurn(statement.get()));
    }
    return turns;
}

std::vector<ArchivedTurn> ConversationArchive::LoadPreviousSessionTail(
    const std::string& currentSessionId,
    const std::size_t maxTurns) const
{
    std::vector<ArchivedTurn> turns;
    sqlite3* const database = Acquire();
    if (database == nullptr)
    {
        return turns;
    }

    std::string previous;
    {
        Statement statement;
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(database,
                "SELECT session_id FROM conversation_sessions WHERE session_id <> ? "
                "ORDER BY CAST(started_at AS INTEGER) DESC LIMIT 1;",
                -1, &raw, nullptr) != SQLITE_OK)
        {
            return turns;
        }
        statement.reset(raw);
        sqlite3_bind_text(
            statement.get(), 1, currentSessionId.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(statement.get()) != SQLITE_ROW)
        {
            return turns;
        }
        previous = ColumnText(statement.get(), 0);
    }

    Statement statement;
    sqlite3_stmt* raw = nullptr;
    const std::string sql = std::string("SELECT ") + TurnColumns +
        " FROM conversation_turns WHERE session_id = ? ORDER BY turn_index DESC LIMIT ?;";
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
    {
        return turns;
    }
    statement.reset(raw);
    sqlite3_bind_text(statement.get(), 1, previous.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(maxTurns));
    while (sqlite3_step(statement.get()) == SQLITE_ROW)
    {
        turns.push_back(ReadTurn(statement.get()));
    }
    // Selected newest-first to take the tail; returned oldest-first because that is the
    // order a conversation is replayed in.
    std::reverse(turns.begin(), turns.end());
    return turns;
}

std::vector<ArchivedSession> ConversationArchive::RecentSessions(
    const std::size_t maxSessions) const
{
    std::vector<ArchivedSession> sessions;
    sqlite3* const database = Acquire();
    if (database == nullptr)
    {
        return sessions;
    }
    Statement statement;
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database,
            "SELECT s.session_id, s.started_at, COALESCE(s.ended_at, ''),"
            "  (SELECT COUNT(*) FROM conversation_turns t WHERE t.session_id = s.session_id),"
            "  COALESCE((SELECT t.content FROM conversation_turns t "
            "     WHERE t.session_id = s.session_id AND t.role = 'user' "
            "     ORDER BY t.turn_index ASC LIMIT 1), '') "
            "FROM conversation_sessions s "
            "ORDER BY CAST(s.started_at AS INTEGER) DESC LIMIT ?;",
            -1, &raw, nullptr) != SQLITE_OK)
    {
        return sessions;
    }
    statement.reset(raw);
    sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(maxSessions));
    while (sqlite3_step(statement.get()) == SQLITE_ROW)
    {
        ArchivedSession session;
        session.id = ColumnText(statement.get(), 0);
        session.startedAt = ColumnText(statement.get(), 1);
        session.endedAt = ColumnText(statement.get(), 2);
        session.turns = static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 3));
        session.opening = ColumnText(statement.get(), 4);
        sessions.push_back(std::move(session));
    }
    return sessions;
}

std::vector<ArchivedTurn> ConversationArchive::Search(
    const std::string& query,
    const std::size_t maxTurns) const
{
    std::vector<ArchivedTurn> turns;
    if (query.empty())
    {
        return turns;
    }
    sqlite3* const database = Acquire();
    if (database == nullptr)
    {
        return turns;
    }
    Statement statement;
    sqlite3_stmt* raw = nullptr;
    const std::string sql =
        "SELECT t.id, t.session_id, t.turn_index, t.role, t.content, t.created_at "
        "FROM conversation_search s "
        "JOIN conversation_turns t ON t.rowid = s.rowid "
        "WHERE conversation_search MATCH ? "
        "ORDER BY bm25(conversation_search) ASC LIMIT ?;";
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
    {
        return turns;
    }
    statement.reset(raw);
    const std::string match = QuoteForFts(query);
    sqlite3_bind_text(statement.get(), 1, match.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(maxTurns));
    while (sqlite3_step(statement.get()) == SQLITE_ROW)
    {
        turns.push_back(ReadTurn(statement.get()));
    }
    return turns;
}

std::size_t ConversationArchive::Forget()
{
    sqlite3* const database = Acquire();
    const std::size_t before = TotalTurns();
    if (database == nullptr)
    {
        return 0;
    }
    if (!Execute(database,
            "DELETE FROM conversation_turns;"
            "DELETE FROM conversation_sessions;"))
    {
        return 0;
    }
    // Reclaims the pages rather than leaving the text sitting in free space where a
    // forget the user asked for would still be recoverable from the file.
    Execute(database, "VACUUM;");
    return before;
}

std::size_t ConversationArchive::ForgetSession(const std::string& sessionId)
{
    sqlite3* const database = Acquire();
    if (database == nullptr)
    {
        return 0;
    }
    std::size_t removed = 0;
    {
        Statement statement;
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(database,
                "SELECT COUNT(*) FROM conversation_turns WHERE session_id = ?;",
                -1, &raw, nullptr) == SQLITE_OK)
        {
            statement.reset(raw);
            sqlite3_bind_text(statement.get(), 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(statement.get()) == SQLITE_ROW)
            {
                removed = static_cast<std::size_t>(
                    sqlite3_column_int64(statement.get(), 0));
            }
        }
    }

    for (const char* sql : {
            "DELETE FROM conversation_turns WHERE session_id = ?;",
            "DELETE FROM conversation_sessions WHERE session_id = ?;"})
    {
        Statement statement;
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
        {
            return removed;
        }
        statement.reset(raw);
        sqlite3_bind_text(statement.get(), 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(statement.get());
    }
    return removed;
}

std::size_t ConversationArchive::TotalTurns() const
{
    sqlite3* const database = Acquire();
    if (database == nullptr)
    {
        return 0;
    }
    Statement statement;
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database,
            "SELECT COUNT(*) FROM conversation_turns;", -1, &raw, nullptr) != SQLITE_OK)
    {
        return 0;
    }
    statement.reset(raw);
    if (sqlite3_step(statement.get()) != SQLITE_ROW)
    {
        return 0;
    }
    return static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 0));
}

ArchiveCounters ConversationArchive::Counters() const
{
    return counters;
}

std::string ConversationArchive::Status() const
{
    std::ostringstream stream;
    // A count, not the sessions themselves. This used to load every session -- each one
    // costing a correlated COUNT and a lookup of its opening line -- and then use only
    // the size of the vector, which is four hundred subqueries to learn one number.
    std::size_t sessionCount = 0;
    if (sqlite3* const database = Acquire(); database != nullptr)
    {
        Statement statement;
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(database,
                "SELECT COUNT(*) FROM conversation_sessions;",
                -1, &raw, nullptr) == SQLITE_OK)
        {
            statement.reset(raw);
            if (sqlite3_step(statement.get()) == SQLITE_ROW)
            {
                sessionCount = static_cast<std::size_t>(
                    sqlite3_column_int64(statement.get(), 0));
            }
        }
    }
    stream << TotalTurns() << " turns archived across " << sessionCount
        << (sessionCount == 1 ? " conversation" : " conversations")
        << ", keeping at most " << limits.maxSessions << " conversations and "
        << limits.maxTurnsPerSession << " turns each.";
    if (counters.withheldSensitive > 0)
    {
        stream << ' ' << counters.withheldSensitive
            << (counters.withheldSensitive == 1 ? " turn was" : " turns were")
            << " withheld this session for matching a sensitive-content marker.";
    }
    if (counters.prunedSessions > 0)
    {
        stream << ' ' << counters.prunedSessions
            << " older conversations were dropped at the retention ceiling.";
    }
    return stream.str();
}

} // namespace revia::memory
