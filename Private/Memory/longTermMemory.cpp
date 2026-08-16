#include "Memory/longTermMemory.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <sqlite3.h>
#include <unordered_set>
#include <unordered_map>
#include <utility>

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

std::string NormalizeSummary(const std::string& value)
{
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char character : value)
    {
        if (std::isalnum(character))
        {
            normalized.push_back(static_cast<char>(std::tolower(character)));
        }
    }
    return normalized;
}

std::unordered_set<std::string> SummaryTokens(const std::string& value)
{
    std::unordered_set<std::string> tokens;
    std::string token;
    for (const unsigned char character : value)
    {
        if (std::isalnum(character))
        {
            token.push_back(static_cast<char>(std::tolower(character)));
        }
        else if (!token.empty())
        {
            tokens.insert(std::move(token));
            token.clear();
        }
    }
    if (!token.empty())
    {
        tokens.insert(std::move(token));
    }
    return tokens;
}

bool IsDuplicateSummary(const std::string& existing, const std::string& candidate)
{
    if (NormalizeSummary(existing) == NormalizeSummary(candidate))
    {
        return true;
    }

    const auto existingTokens = SummaryTokens(existing);
    const auto candidateTokens = SummaryTokens(candidate);
    const std::size_t smallerSize = std::min(existingTokens.size(), candidateTokens.size());
    if (smallerSize < 4)
    {
        return false;
    }

    std::size_t sharedTokens = 0;
    for (const std::string& token : existingTokens)
    {
        if (candidateTokens.contains(token))
        {
            ++sharedTokens;
        }
    }
    return static_cast<double>(sharedTokens) / static_cast<double>(smallerSize) >= 0.9;
}

std::string CurrentEpochSeconds()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

std::string BuildMemoryId(const std::string& summary, const std::string& createdAt)
{
    return "memory-" + createdAt + "-" + std::to_string(std::hash<std::string>{}(summary));
}

bool Execute(sqlite3* database, const char* sql)
{
    return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

Statement Prepare(sqlite3* database, const char* sql)
{
    sqlite3_stmt* rawStatement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr) != SQLITE_OK)
    {
        return {};
    }
    return Statement(rawStatement);
}

void BindText(sqlite3_stmt* statement, const int index, const std::string& value)
{
    sqlite3_bind_text(
        statement,
        index,
        value.c_str(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT);
}

bool InsertEntry(sqlite3* database, const memoryEntry& entry)
{
    Statement insert = Prepare(database,
        "INSERT OR IGNORE INTO memories "
        "(id, category, summary, normalized_summary, source, created_at, active) "
        "VALUES (?, ?, ?, ?, ?, ?, 1);");
    if (!insert)
    {
        return false;
    }

    BindText(insert.get(), 1, entry.id);
    BindText(insert.get(), 2, entry.category.empty() ? "other" : entry.category);
    BindText(insert.get(), 3, entry.summary);
    BindText(insert.get(), 4, NormalizeSummary(entry.summary));
    BindText(insert.get(), 5, entry.source.empty() ? "automatic" : entry.source);
    BindText(insert.get(), 6, entry.createdAt);
    return sqlite3_step(insert.get()) == SQLITE_DONE;
}

bool IsValidEmbedding(const std::vector<float>& embedding)
{
    return !embedding.empty() && std::all_of(
        embedding.begin(),
        embedding.end(),
        [](const float value)
        {
            return std::isfinite(value);
        });
}

bool UpsertEmbedding(
    sqlite3* database,
    const std::string& memoryId,
    const std::string& model,
    const std::vector<float>& embedding)
{
    if (memoryId.empty() || model.empty() || !IsValidEmbedding(embedding))
    {
        return false;
    }

    Statement insert = Prepare(database,
        "INSERT INTO memory_embeddings(memory_id, model, dimensions, vector, created_at) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(memory_id, model) DO UPDATE SET "
        "dimensions = excluded.dimensions, vector = excluded.vector, "
        "created_at = excluded.created_at;");
    if (!insert)
    {
        return false;
    }

    BindText(insert.get(), 1, memoryId);
    BindText(insert.get(), 2, model);
    sqlite3_bind_int64(insert.get(), 3, static_cast<sqlite3_int64>(embedding.size()));
    sqlite3_bind_blob64(
        insert.get(),
        4,
        embedding.data(),
        static_cast<sqlite3_uint64>(embedding.size() * sizeof(float)),
        SQLITE_TRANSIENT);
    BindText(insert.get(), 5, CurrentEpochSeconds());
    return sqlite3_step(insert.get()) == SQLITE_DONE;
}

bool WasLegacyMemoryImported(sqlite3* database)
{
    Statement query = Prepare(database,
        "SELECT 1 FROM memory_metadata WHERE key = 'legacy_jsonl_imported' LIMIT 1;");
    return query && sqlite3_step(query.get()) == SQLITE_ROW;
}

bool MarkLegacyMemoryImported(sqlite3* database)
{
    return Execute(database,
        "INSERT OR REPLACE INTO memory_metadata(key, value) "
        "VALUES ('legacy_jsonl_imported', '1');");
}

bool ImportLegacyJsonl(sqlite3* database, const std::filesystem::path& databasePath)
{
    if (WasLegacyMemoryImported(database))
    {
        return true;
    }

    std::filesystem::path legacyPath = databasePath;
    legacyPath.replace_extension(".jsonl");
    std::ifstream file(legacyPath);
    if (!file.is_open())
    {
        return MarkLegacyMemoryImported(database);
    }

    if (!Execute(database, "BEGIN IMMEDIATE;"))
    {
        return false;
    }

    bool imported = true;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty())
        {
            continue;
        }

        try
        {
            const nlohmann::json data = nlohmann::json::parse(line);
            if (!data.contains("summary") || !data["summary"].is_string())
            {
                continue;
            }

            memoryEntry entry;
            entry.summary = data["summary"].get<std::string>();
            if (entry.summary.empty())
            {
                continue;
            }
            entry.category = data.value("category", "other");
            entry.source = data.value("source", "automatic");
            entry.createdAt = data.value("createdAt", CurrentEpochSeconds());
            entry.id = data.value("id", BuildMemoryId(entry.summary, entry.createdAt));
            if (!InsertEntry(database, entry))
            {
                imported = false;
                break;
            }
        }
        catch (const std::exception&)
        {
            // Keep importing valid structured records around a malformed line.
        }
    }

    if (imported)
    {
        imported = MarkLegacyMemoryImported(database);
    }
    if (imported)
    {
        return Execute(database, "COMMIT;");
    }

    Execute(database, "ROLLBACK;");
    return false;
}

Database OpenDatabase(const std::string& memoryPath)
{
    const std::filesystem::path path(memoryPath);
    std::error_code directoryError;
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path(), directoryError);
        if (directoryError)
        {
            return {};
        }
    }

    sqlite3* rawDatabase = nullptr;
    if (sqlite3_open_v2(
        memoryPath.c_str(),
        &rawDatabase,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr) != SQLITE_OK)
    {
        if (rawDatabase != nullptr)
        {
            sqlite3_close(rawDatabase);
        }
        return {};
    }

    Database database(rawDatabase);
    sqlite3_busy_timeout(database.get(), 2000);
    constexpr const char* Schema =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS memories ("
        "  id TEXT NOT NULL UNIQUE,"
        "  category TEXT NOT NULL,"
        "  summary TEXT NOT NULL,"
        "  normalized_summary TEXT NOT NULL UNIQUE,"
        "  source TEXT NOT NULL,"
        "  created_at TEXT NOT NULL,"
        "  active INTEGER NOT NULL DEFAULT 1"
        ");"
        "CREATE VIRTUAL TABLE IF NOT EXISTS memory_search USING fts5("
        "  summary, category, content='memories', content_rowid='rowid',"
        "  tokenize='unicode61 remove_diacritics 2'"
        ");"
        "CREATE TRIGGER IF NOT EXISTS memories_after_insert AFTER INSERT ON memories BEGIN "
        "  INSERT INTO memory_search(rowid, summary, category) "
        "  VALUES (new.rowid, new.summary, new.category);"
        "END;"
        "CREATE TRIGGER IF NOT EXISTS memories_after_delete AFTER DELETE ON memories BEGIN "
        "  INSERT INTO memory_search(memory_search, rowid, summary, category) "
        "  VALUES ('delete', old.rowid, old.summary, old.category);"
        "END;"
        "CREATE TRIGGER IF NOT EXISTS memories_after_update AFTER UPDATE ON memories BEGIN "
        "  INSERT INTO memory_search(memory_search, rowid, summary, category) "
        "  VALUES ('delete', old.rowid, old.summary, old.category);"
        "  INSERT INTO memory_search(rowid, summary, category) "
        "  VALUES (new.rowid, new.summary, new.category);"
        "END;"
        "CREATE TABLE IF NOT EXISTS memory_metadata ("
        "  key TEXT PRIMARY KEY, value TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS memory_embeddings ("
        "  memory_id TEXT NOT NULL,"
        "  model TEXT NOT NULL,"
        "  dimensions INTEGER NOT NULL,"
        "  vector BLOB NOT NULL,"
        "  created_at TEXT NOT NULL,"
        "  PRIMARY KEY(memory_id, model),"
        "  FOREIGN KEY(memory_id) REFERENCES memories(id) ON DELETE CASCADE"
        ");";
    if (!Execute(database.get(), Schema) || !ImportLegacyJsonl(database.get(), path))
    {
        return {};
    }
    return database;
}

memoryEntry ReadEntry(sqlite3_stmt* statement)
{
    const auto text = [&](const int column)
    {
        const unsigned char* value = sqlite3_column_text(statement, column);
        return value == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(value));
    };

    memoryEntry entry;
    entry.id = text(0);
    entry.category = text(1);
    entry.summary = text(2);
    entry.source = text(3);
    entry.createdAt = text(4);
    return entry;
}

std::string BuildFtsQuery(const std::string& query)
{
    static const std::unordered_set<std::string> StopWords = {
        "a", "an", "and", "are", "about", "can", "could", "did", "do", "does", "for",
        "how", "i", "in", "is", "it", "me", "my", "of", "on", "or", "should", "that",
        "the", "this", "to", "user", "what", "when", "where", "which", "who", "why", "with",
        "would", "you", "your"
    };

    std::vector<std::string> tokens;
    std::unordered_set<std::string> seen;
    std::string token;
    const auto finishToken = [&]()
    {
        if (token.size() >= 2 && !StopWords.contains(token) && seen.insert(token).second)
        {
            tokens.push_back(token);
        }
        token.clear();
    };

    for (const unsigned char character : query)
    {
        if (std::isalnum(character))
        {
            token.push_back(static_cast<char>(std::tolower(character)));
        }
        else
        {
            finishToken();
        }
    }
    finishToken();

    std::ostringstream expression;
    for (std::size_t index = 0; index < tokens.size(); ++index)
    {
        if (index > 0)
        {
            expression << " OR ";
        }
        expression << '"' << tokens[index] << "\"*";
    }
    return expression.str();
}

} // namespace

longTermMemory::longTermMemory(std::string path) : memoryPath(std::move(path)) {}

longTermMemory::~longTermMemory() = default;

std::vector<memoryEntry> longTermMemory::Load() const
{
    Database database = OpenDatabase(memoryPath);
    if (!database)
    {
        return {};
    }

    Statement query = Prepare(database.get(),
        "SELECT id, category, summary, source, created_at "
        "FROM memories WHERE active = 1 "
        "ORDER BY CAST(created_at AS INTEGER), rowid;");
    if (!query)
    {
        return {};
    }

    std::vector<memoryEntry> entries;
    while (sqlite3_step(query.get()) == SQLITE_ROW)
    {
        entries.push_back(ReadEntry(query.get()));
    }
    return entries;
}

bool longTermMemory::Save(const memoryDecision& decision, bool& outWasAdded) const
{
    outWasAdded = false;
    if (!decision.bSuccess || !decision.bShouldRemember || decision.summary.empty())
    {
        return false;
    }

    const std::vector<memoryEntry> existingEntries = Load();
    const auto duplicate = std::find_if(
        existingEntries.begin(),
        existingEntries.end(),
        [&](const memoryEntry& entry)
        {
            return IsDuplicateSummary(entry.summary, decision.summary);
        });
    if (duplicate != existingEntries.end())
    {
        if (!decision.embedding.empty() && !decision.embeddingModel.empty())
        {
            return SaveEmbedding(
                duplicate->id,
                decision.embeddingModel,
                decision.embedding);
        }
        return true;
    }

    Database database = OpenDatabase(memoryPath);
    if (!database)
    {
        return false;
    }

    const std::string createdAt = CurrentEpochSeconds();
    memoryEntry entry;
    entry.id = BuildMemoryId(decision.summary, createdAt);
    entry.category = decision.category.empty() ? "other" : decision.category;
    entry.summary = decision.summary;
    entry.source = "automatic";
    entry.createdAt = createdAt;

    if (!InsertEntry(database.get(), entry))
    {
        return false;
    }
    outWasAdded = sqlite3_changes(database.get()) > 0;
    if (outWasAdded && !decision.embedding.empty() && !decision.embeddingModel.empty() &&
        !UpsertEmbedding(
            database.get(),
            entry.id,
            decision.embeddingModel,
            decision.embedding))
    {
        return false;
    }
    return true;
}

bool longTermMemory::HasMemories() const
{
    Database database = OpenDatabase(memoryPath);
    if (!database)
    {
        return false;
    }

    Statement query = Prepare(database.get(),
        "SELECT 1 FROM memories WHERE active = 1 LIMIT 1;");
    return query && sqlite3_step(query.get()) == SQLITE_ROW;
}

std::vector<memoryEntry> longTermMemory::Search(
    const std::string& queryText,
    const std::size_t maxEntries,
    const std::vector<float>& queryEmbedding,
    const std::string& embeddingModel) const
{
    if (maxEntries == 0 || (queryText.empty() && queryEmbedding.empty()))
    {
        return {};
    }

    Database database = OpenDatabase(memoryPath);
    if (!database)
    {
        return {};
    }

    const std::size_t candidateLimit = std::max<std::size_t>(maxEntries * 4, 20);
    std::vector<memoryEntry> lexicalEntries;
    const std::string ftsQuery = BuildFtsQuery(queryText);
    if (!ftsQuery.empty())
    {
        Statement lexicalQuery = Prepare(database.get(),
            "SELECT memories.id, memories.category, memories.summary, "
            "       memories.source, memories.created_at "
            "FROM memory_search "
            "JOIN memories ON memories.rowid = memory_search.rowid "
            "WHERE memory_search MATCH ? AND memories.active = 1 "
            "ORDER BY bm25(memory_search, 6.0, 1.0), "
            "         CAST(memories.created_at AS INTEGER) DESC "
            "LIMIT ?;");
        if (lexicalQuery)
        {
            BindText(lexicalQuery.get(), 1, ftsQuery);
            sqlite3_bind_int64(
                lexicalQuery.get(),
                2,
                static_cast<sqlite3_int64>(candidateLimit));
            while (sqlite3_step(lexicalQuery.get()) == SQLITE_ROW)
            {
                lexicalEntries.push_back(ReadEntry(lexicalQuery.get()));
            }
        }
    }

    struct SemanticCandidate
    {
        memoryEntry entry;
        float similarity = 0.0f;
    };
    std::vector<SemanticCandidate> semanticEntries;
    if (IsValidEmbedding(queryEmbedding) && !embeddingModel.empty())
    {
        Statement semanticQuery = Prepare(database.get(),
            "SELECT memories.id, memories.category, memories.summary, "
            "       memories.source, memories.created_at, "
            "       memory_embeddings.dimensions, memory_embeddings.vector "
            "FROM memory_embeddings "
            "JOIN memories ON memories.id = memory_embeddings.memory_id "
            "WHERE memory_embeddings.model = ? AND memories.active = 1;");
        if (semanticQuery)
        {
            BindText(semanticQuery.get(), 1, embeddingModel);
            while (sqlite3_step(semanticQuery.get()) == SQLITE_ROW)
            {
                const sqlite3_int64 dimensions = sqlite3_column_int64(semanticQuery.get(), 5);
                const int byteCount = sqlite3_column_bytes(semanticQuery.get(), 6);
                const void* blob = sqlite3_column_blob(semanticQuery.get(), 6);
                if (dimensions != static_cast<sqlite3_int64>(queryEmbedding.size()) ||
                    byteCount != static_cast<int>(queryEmbedding.size() * sizeof(float)) ||
                    blob == nullptr)
                {
                    continue;
                }

                std::vector<float> stored(queryEmbedding.size());
                std::memcpy(stored.data(), blob, static_cast<std::size_t>(byteCount));
                double dot = 0.0;
                double queryLength = 0.0;
                double storedLength = 0.0;
                for (std::size_t index = 0; index < queryEmbedding.size(); ++index)
                {
                    dot += static_cast<double>(queryEmbedding[index]) * stored[index];
                    queryLength += static_cast<double>(queryEmbedding[index]) * queryEmbedding[index];
                    storedLength += static_cast<double>(stored[index]) * stored[index];
                }
                if (queryLength <= 0.0 || storedLength <= 0.0)
                {
                    continue;
                }

                const float similarity = static_cast<float>(
                    dot / (std::sqrt(queryLength) * std::sqrt(storedLength)));
                if (std::isfinite(similarity) && similarity >= 0.35f)
                {
                    semanticEntries.push_back({ReadEntry(semanticQuery.get()), similarity});
                }
            }
        }
        std::sort(
            semanticEntries.begin(),
            semanticEntries.end(),
            [](const SemanticCandidate& left, const SemanticCandidate& right)
            {
                return left.similarity > right.similarity;
            });
        if (semanticEntries.size() > candidateLimit)
        {
            semanticEntries.resize(candidateLimit);
        }
    }

    struct CombinedCandidate
    {
        memoryEntry entry;
        double score = 0.0;
    };
    std::vector<CombinedCandidate> combined;
    std::unordered_map<std::string, std::size_t> positions;
    const auto addCandidate = [&](const memoryEntry& entry, const double score)
    {
        const auto existing = positions.find(entry.id);
        if (existing == positions.end())
        {
            positions.emplace(entry.id, combined.size());
            combined.push_back({entry, score});
        }
        else
        {
            combined[existing->second].score += score;
        }
    };

    constexpr double RrfConstant = 60.0;
    for (std::size_t rank = 0; rank < lexicalEntries.size(); ++rank)
    {
        addCandidate(lexicalEntries[rank], 1.0 / (RrfConstant + rank + 1.0));
    }
    for (std::size_t rank = 0; rank < semanticEntries.size(); ++rank)
    {
        addCandidate(
            semanticEntries[rank].entry,
            1.25 / (RrfConstant + rank + 1.0));
    }

    std::sort(
        combined.begin(),
        combined.end(),
        [](const CombinedCandidate& left, const CombinedCandidate& right)
        {
            if (left.score != right.score)
            {
                return left.score > right.score;
            }
            return left.entry.createdAt > right.entry.createdAt;
        });
    if (combined.size() > maxEntries)
    {
        combined.resize(maxEntries);
    }

    std::vector<memoryEntry> results;
    results.reserve(combined.size());
    for (CombinedCandidate& candidate : combined)
    {
        results.push_back(std::move(candidate.entry));
    }
    return results;
}

std::string longTermMemory::BuildPromptBlock(
    const std::string& query,
    const std::size_t maxEntries,
    const std::vector<float>& queryEmbedding,
    const std::string& embeddingModel) const
{
    if (maxEntries == 0)
    {
        return "";
    }

    std::vector<memoryEntry> entries = query.empty()
        ? Load()
        : Search(query, maxEntries, queryEmbedding, embeddingModel);
    if (entries.empty())
    {
        return "";
    }
    if (query.empty() && entries.size() > maxEntries)
    {
        entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(maxEntries));
    }

    std::ostringstream stream;
    stream << "Retrieved long-term user memory. Use it only when relevant, and prefer the user's "
              "current statement if anything conflicts:\n";
    for (const memoryEntry& entry : entries)
    {
        stream << "- [" << entry.category << "] " << entry.summary << "\n";
    }
    return stream.str();
}

std::vector<memoryEntry> longTermMemory::LoadMissingEmbeddings(
    const std::string& embeddingModel,
    const std::size_t maxEntries) const
{
    if (embeddingModel.empty() || maxEntries == 0)
    {
        return {};
    }

    Database database = OpenDatabase(memoryPath);
    if (!database)
    {
        return {};
    }

    Statement query = Prepare(database.get(),
        "SELECT memories.id, memories.category, memories.summary, "
        "       memories.source, memories.created_at "
        "FROM memories "
        "LEFT JOIN memory_embeddings ON "
        "  memory_embeddings.memory_id = memories.id AND memory_embeddings.model = ? "
        "WHERE memories.active = 1 AND memory_embeddings.memory_id IS NULL "
        "ORDER BY CAST(memories.created_at AS INTEGER), memories.rowid "
        "LIMIT ?;");
    if (!query)
    {
        return {};
    }
    BindText(query.get(), 1, embeddingModel);
    sqlite3_bind_int64(query.get(), 2, static_cast<sqlite3_int64>(maxEntries));

    std::vector<memoryEntry> entries;
    while (sqlite3_step(query.get()) == SQLITE_ROW)
    {
        entries.push_back(ReadEntry(query.get()));
    }
    return entries;
}

bool longTermMemory::SaveEmbedding(
    const std::string& memoryId,
    const std::string& embeddingModel,
    const std::vector<float>& embedding) const
{
    Database database = OpenDatabase(memoryPath);
    return database && UpsertEmbedding(
        database.get(),
        memoryId,
        embeddingModel,
        embedding);
}
