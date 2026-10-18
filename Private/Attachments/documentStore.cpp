#include "Attachments/documentStore.h"

#include "Actions/actionTypes.h"
#include "Memory/hybridRanking.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <system_error>
#include <unordered_map>

namespace revia::attachments
{
namespace
{
struct StatementCloser
{
    void operator()(sqlite3_stmt* statement) const { sqlite3_finalize(statement); }
};
using Statement = std::unique_ptr<sqlite3_stmt, StatementCloser>;

bool Execute(sqlite3* database, const char* sql)
{
    char* message = nullptr;
    const int status = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    sqlite3_free(message);
    return status == SQLITE_OK;
}

Statement Prepare(sqlite3* database, const std::string& sql)
{
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(raw);
        return {};
    }
    return Statement(raw);
}

void BindText(sqlite3_stmt* statement, const int index, const std::string& value)
{
    sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string ColumnText(sqlite3_stmt* statement, const int column)
{
    const unsigned char* value = sqlite3_column_text(statement, column);
    return value == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(value),
                                                  static_cast<std::size_t>(sqlite3_column_bytes(statement, column)));
}

AttachmentKind KindFrom(const std::string& text)
{
    if (text == "image")
        return AttachmentKind::Image;
    if (text == "pdf")
        return AttachmentKind::Pdf;
    if (text == "docx")
        return AttachmentKind::Docx;
    return AttachmentKind::Text;
}

// "AND c.document_id IN (?3, ?4, ...)" for a restriction, bound from `first`.
std::string RestrictionClause(const std::vector<std::string>& restrictTo, const int first)
{
    if (restrictTo.empty())
        return {};
    std::string clause = " AND c.document_id IN (";
    for (std::size_t index = 0; index < restrictTo.size(); ++index)
        clause += (index ? ", ?" : "?") + std::to_string(first + static_cast<int>(index));
    return clause + ")";
}

void BindRestriction(sqlite3_stmt* statement, const std::vector<std::string>& restrictTo, const int first)
{
    for (std::size_t index = 0; index < restrictTo.size(); ++index)
        BindText(statement, first + static_cast<int>(index), restrictTo[index]);
}

class Transaction
{
public:
    explicit Transaction(sqlite3* database) : database(database), active(Execute(database, "BEGIN IMMEDIATE;")) {}
    ~Transaction()
    {
        if (active)
            Execute(database, "ROLLBACK;");
    }
    [[nodiscard]] bool IsActive() const { return active; }
    bool Commit()
    {
        if (!active || !Execute(database, "COMMIT;"))
            return false;
        active = false;
        return true;
    }

private:
    sqlite3* database;
    bool active;
};

constexpr const char* Schema =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "PRAGMA foreign_keys=ON;"
    // Freed pages are overwritten, so a forgotten document's text does not linger in
    // free space between the delete and the vacuum.
    "PRAGMA secure_delete=ON;"
    "CREATE TABLE IF NOT EXISTS documents ("
    "  id TEXT PRIMARY KEY,"
    "  name TEXT NOT NULL,"
    "  kind TEXT NOT NULL,"
    "  mime TEXT NOT NULL,"
    "  bytes INTEGER NOT NULL,"
    "  digest TEXT NOT NULL,"
    "  pages INTEGER NOT NULL,"
    "  chunk_count INTEGER NOT NULL,"
    "  stored_path TEXT NOT NULL,"
    "  added_at INTEGER NOT NULL"
    ");"
    // An explicit INTEGER PRIMARY KEY: VACUUM may renumber an implicit rowid, which
    // would detach the search index and the vectors from their chunks.
    "CREATE TABLE IF NOT EXISTS document_chunks ("
    "  id INTEGER PRIMARY KEY,"
    "  document_id TEXT NOT NULL REFERENCES documents(id) ON DELETE CASCADE,"
    "  chunk_index INTEGER NOT NULL,"
    "  page INTEGER NOT NULL,"
    "  text TEXT NOT NULL,"
    "  UNIQUE(document_id, chunk_index)"
    ");"
    "CREATE VIRTUAL TABLE IF NOT EXISTS document_search USING fts5("
    "  text, content='document_chunks', content_rowid='id',"
    "  tokenize='unicode61 remove_diacritics 2'"
    ");"
    "CREATE TRIGGER IF NOT EXISTS document_chunks_after_insert AFTER INSERT ON document_chunks BEGIN "
    "  INSERT INTO document_search(rowid, text) VALUES (new.id, new.text);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS document_chunks_after_delete AFTER DELETE ON document_chunks BEGIN "
    "  INSERT INTO document_search(document_search, rowid, text) VALUES ('delete', old.id, old.text);"
    "END;"
    "CREATE TABLE IF NOT EXISTS document_embeddings ("
    "  chunk_rowid INTEGER NOT NULL,"
    "  model TEXT NOT NULL,"
    "  dimensions INTEGER NOT NULL,"
    "  vector BLOB NOT NULL,"
    "  PRIMARY KEY(chunk_rowid, model)"
    ");";
} // namespace

DocumentStore::DocumentStore(std::filesystem::path databasePath) : path(std::move(databasePath)) {}

DocumentStore::~DocumentStore()
{
    if (database != nullptr)
        sqlite3_close(database);
}

sqlite3* DocumentStore::Acquire() const
{
    if (database != nullptr)
        return database;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    sqlite3* opened = nullptr;
    if (sqlite3_open_v2(revia::actions::PathToUtf8(path).c_str(), &opened, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK)
    {
        sqlite3_close(opened);
        return nullptr;
    }
    sqlite3_busy_timeout(opened, 2000);
    if (!Execute(opened, Schema))
    {
        sqlite3_close(opened);
        return nullptr;
    }
    database = opened;
    return database;
}

bool DocumentStore::Add(const StoredDocument& document, const std::vector<DocumentChunk>& chunks, std::string& error)
{
    const std::lock_guard lock(mutex);
    sqlite3* const db = Acquire();
    if (db == nullptr)
    {
        error = "The document library could not be opened.";
        return false;
    }
    Transaction transaction(db);
    Statement insert = Prepare(db,
        "INSERT INTO documents(id, name, kind, mime, bytes, digest, pages, chunk_count, stored_path, added_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);");
    if (!transaction.IsActive() || !insert)
    {
        error = "The document library is busy.";
        return false;
    }
    const std::int64_t added = document.addedAt > 0 ? document.addedAt
                                                    : std::chrono::duration_cast<std::chrono::seconds>(
                                                          std::chrono::system_clock::now().time_since_epoch()).count();
    BindText(insert.get(), 1, document.id);
    BindText(insert.get(), 2, document.name);
    BindText(insert.get(), 3, ToString(document.kind));
    BindText(insert.get(), 4, document.mimeType);
    sqlite3_bind_int64(insert.get(), 5, static_cast<sqlite3_int64>(document.bytes));
    BindText(insert.get(), 6, document.digest);
    sqlite3_bind_int64(insert.get(), 7, static_cast<sqlite3_int64>(document.pages));
    sqlite3_bind_int64(insert.get(), 8, static_cast<sqlite3_int64>(chunks.size()));
    BindText(insert.get(), 9, revia::actions::PathToUtf8(document.storedPath));
    sqlite3_bind_int64(insert.get(), 10, added);
    if (sqlite3_step(insert.get()) != SQLITE_DONE)
    {
        error = "The document could not be recorded.";
        return false;
    }
    Statement chunk = Prepare(db, "INSERT INTO document_chunks(document_id, chunk_index, page, text) VALUES (?1, ?2, ?3, ?4);");
    if (!chunk)
    {
        error = "The document's passages could not be stored.";
        return false;
    }
    for (const DocumentChunk& piece : chunks)
    {
        sqlite3_reset(chunk.get());
        BindText(chunk.get(), 1, document.id);
        sqlite3_bind_int64(chunk.get(), 2, static_cast<sqlite3_int64>(piece.index));
        sqlite3_bind_int64(chunk.get(), 3, static_cast<sqlite3_int64>(piece.page));
        BindText(chunk.get(), 4, piece.text);
        if (sqlite3_step(chunk.get()) != SQLITE_DONE)
        {
            error = "The document's passages could not be stored.";
            return false;
        }
    }
    if (!transaction.Commit())
    {
        error = "The document could not be saved.";
        return false;
    }
    return true;
}

std::vector<PendingEmbedding> DocumentStore::MissingEmbeddings(const std::string& model, const std::size_t limit,
    const std::string& documentId) const
{
    const std::lock_guard lock(mutex);
    std::vector<PendingEmbedding> pending;
    sqlite3* const db = Acquire();
    if (db == nullptr || model.empty() || limit == 0)
        return pending;
    Statement query = Prepare(db,
        "SELECT c.rowid, c.text FROM document_chunks c "
        "WHERE NOT EXISTS (SELECT 1 FROM document_embeddings e WHERE e.chunk_rowid = c.rowid AND e.model = ?1) "
        "AND (?2 = '' OR c.document_id = ?2) ORDER BY c.rowid LIMIT ?3;");
    if (!query)
        return pending;
    BindText(query.get(), 1, model);
    BindText(query.get(), 2, documentId);
    sqlite3_bind_int64(query.get(), 3, static_cast<sqlite3_int64>(limit));
    while (sqlite3_step(query.get()) == SQLITE_ROW)
        pending.push_back({sqlite3_column_int64(query.get(), 0), ColumnText(query.get(), 1)});
    return pending;
}

bool DocumentStore::SetEmbedding(const std::int64_t chunkRowId, const std::vector<float>& vector, const std::string& model)
{
    if (model.empty() || !memory::IsValidEmbedding(vector))
        return false;
    const std::lock_guard lock(mutex);
    sqlite3* const db = Acquire();
    if (db == nullptr)
        return false;
    // Only for a chunk that still exists: a document forgotten while it was being
    // embedded must not leave a vector behind.
    Statement insert = Prepare(db,
        "INSERT OR REPLACE INTO document_embeddings(chunk_rowid, model, dimensions, vector) "
        "SELECT ?1, ?2, ?3, ?4 WHERE EXISTS (SELECT 1 FROM document_chunks WHERE rowid = ?1);");
    if (!insert)
        return false;
    sqlite3_bind_int64(insert.get(), 1, chunkRowId);
    BindText(insert.get(), 2, model);
    sqlite3_bind_int64(insert.get(), 3, static_cast<sqlite3_int64>(vector.size()));
    sqlite3_bind_blob(insert.get(), 4, vector.data(), static_cast<int>(vector.size() * sizeof(float)), SQLITE_TRANSIENT);
    return sqlite3_step(insert.get()) == SQLITE_DONE && sqlite3_changes(db) > 0;
}

std::vector<DocumentHit> DocumentStore::Search(const std::string& query, const std::vector<float>& queryEmbedding,
    const std::string& embeddingModel, const std::size_t maxHits, const std::vector<std::string>& restrictTo) const
{
    const std::lock_guard lock(mutex);
    std::vector<DocumentHit> hits;
    sqlite3* const db = Acquire();
    if (db == nullptr || maxHits == 0 || restrictTo.size() > 64)
        return hits;
    const std::size_t candidateLimit = std::max<std::size_t>(maxHits * 4, 20);

    std::vector<std::string> lexical;
    if (const std::string fts = memory::BuildFtsQuery(query); !fts.empty())
    {
        Statement statement = Prepare(db,
            "SELECT c.rowid FROM document_search JOIN document_chunks c ON c.rowid = document_search.rowid "
            "WHERE document_search MATCH ?1" + RestrictionClause(restrictTo, 3) +
            " ORDER BY bm25(document_search), c.rowid LIMIT ?2;");
        if (statement)
        {
            BindText(statement.get(), 1, fts);
            sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(candidateLimit));
            BindRestriction(statement.get(), restrictTo, 3);
            while (sqlite3_step(statement.get()) == SQLITE_ROW)
                lexical.push_back(std::to_string(sqlite3_column_int64(statement.get(), 0)));
        }
    }

    std::vector<std::string> semantic;
    const double queryNorm = memory::VectorNorm(queryEmbedding);
    if (memory::IsValidEmbedding(queryEmbedding) && !embeddingModel.empty() && queryNorm > 0.0)
    {
        Statement statement = Prepare(db,
            "SELECT c.rowid, e.dimensions, e.vector FROM document_embeddings e JOIN document_chunks c ON c.rowid = e.chunk_rowid "
            "WHERE e.model = ?1" + RestrictionClause(restrictTo, 2) + ";");
        if (statement)
        {
            BindText(statement.get(), 1, embeddingModel);
            BindRestriction(statement.get(), restrictTo, 2);
            std::vector<std::pair<float, std::int64_t>> scored;
            while (sqlite3_step(statement.get()) == SQLITE_ROW)
            {
                const auto similarity = memory::CosineSimilarity(queryEmbedding, queryNorm, sqlite3_column_blob(statement.get(), 2),
                    sqlite3_column_bytes(statement.get(), 2), sqlite3_column_int64(statement.get(), 1));
                if (similarity && *similarity >= memory::MinimumSemanticSimilarity)
                    scored.emplace_back(*similarity, sqlite3_column_int64(statement.get(), 0));
            }
            std::sort(scored.begin(), scored.end(),
                [](const auto& left, const auto& right) { return left.first != right.first ? left.first > right.first : left.second < right.second; });
            for (std::size_t index = 0; index < scored.size() && index < candidateLimit; ++index)
                semantic.push_back(std::to_string(scored[index].second));
        }
    }

    // The same weights long-term memory gives its lexical and semantic lists.
    std::vector<memory::FusedRank> fused = memory::FuseReciprocalRanks({{lexical, 1.0}, {semantic, 1.25}});
    std::sort(fused.begin(), fused.end(),
        [](const memory::FusedRank& left, const memory::FusedRank& right)
        { return left.score != right.score ? left.score > right.score : left.firstSeen < right.firstSeen; });
    if (fused.size() > maxHits)
        fused.resize(maxHits);

    Statement detail = Prepare(db,
        "SELECT c.document_id, d.name, c.chunk_index, c.page, c.text FROM document_chunks c "
        "JOIN documents d ON d.id = c.document_id WHERE c.rowid = ?1;");
    if (!detail)
        return hits;
    for (const memory::FusedRank& rank : fused)
    {
        sqlite3_reset(detail.get());
        sqlite3_bind_int64(detail.get(), 1, std::stoll(rank.key));
        if (sqlite3_step(detail.get()) != SQLITE_ROW)
            continue;
        hits.push_back({ColumnText(detail.get(), 0), ColumnText(detail.get(), 1), static_cast<std::size_t>(sqlite3_column_int64(detail.get(), 2)),
            static_cast<std::size_t>(sqlite3_column_int64(detail.get(), 3)), ColumnText(detail.get(), 4), rank.score});
    }
    return hits;
}

std::optional<DocumentHit> DocumentStore::ChunkAt(const std::string& documentId, const std::size_t chunkIndex) const
{
    const std::lock_guard lock(mutex);
    sqlite3* const db = Acquire();
    if (db == nullptr)
        return std::nullopt;
    Statement query = Prepare(db,
        "SELECT d.name, c.page, c.text FROM document_chunks c JOIN documents d ON d.id = c.document_id "
        "WHERE c.document_id = ?1 AND c.chunk_index = ?2;");
    if (!query)
        return std::nullopt;
    BindText(query.get(), 1, documentId);
    sqlite3_bind_int64(query.get(), 2, static_cast<sqlite3_int64>(chunkIndex));
    if (sqlite3_step(query.get()) != SQLITE_ROW)
        return std::nullopt;
    return DocumentHit{documentId, ColumnText(query.get(), 0), chunkIndex, static_cast<std::size_t>(sqlite3_column_int64(query.get(), 1)),
        ColumnText(query.get(), 2), 0.0};
}

std::vector<StoredDocument> DocumentStore::List(const std::string& embeddingModel) const
{
    const std::lock_guard lock(mutex);
    std::vector<StoredDocument> documents;
    sqlite3* const db = Acquire();
    if (db == nullptr)
        return documents;
    Statement query = Prepare(db,
        "SELECT d.id, d.name, d.kind, d.mime, d.bytes, d.digest, d.pages, d.chunk_count, d.stored_path, d.added_at, "
        "  (SELECT COUNT(*) FROM document_chunks c JOIN document_embeddings e ON e.chunk_rowid = c.rowid "
        "   WHERE c.document_id = d.id AND e.model = ?1) "
        "FROM documents d ORDER BY d.added_at DESC, d.rowid DESC;");
    if (!query)
        return documents;
    BindText(query.get(), 1, embeddingModel);
    while (sqlite3_step(query.get()) == SQLITE_ROW)
    {
        StoredDocument document;
        document.id = ColumnText(query.get(), 0);
        document.name = ColumnText(query.get(), 1);
        document.kind = KindFrom(ColumnText(query.get(), 2));
        document.mimeType = ColumnText(query.get(), 3);
        document.bytes = static_cast<std::uintmax_t>(sqlite3_column_int64(query.get(), 4));
        document.digest = ColumnText(query.get(), 5);
        document.pages = static_cast<std::size_t>(sqlite3_column_int64(query.get(), 6));
        document.chunks = static_cast<std::size_t>(sqlite3_column_int64(query.get(), 7));
        document.storedPath = revia::actions::Utf8ToPath(ColumnText(query.get(), 8));
        document.addedAt = sqlite3_column_int64(query.get(), 9);
        document.embeddedChunks = static_cast<std::size_t>(sqlite3_column_int64(query.get(), 10));
        documents.push_back(std::move(document));
    }
    return documents;
}

std::optional<StoredDocument> DocumentStore::Find(const std::string& id) const
{
    for (StoredDocument& document : List())
        if (document.id == id)
            return std::move(document);
    return std::nullopt;
}

bool DocumentStore::HasDocuments() const
{
    const std::lock_guard lock(mutex);
    sqlite3* const db = Acquire();
    Statement query = db ? Prepare(db, "SELECT 1 FROM document_chunks LIMIT 1;") : Statement();
    return query && sqlite3_step(query.get()) == SQLITE_ROW;
}

bool DocumentStore::Forget(const std::string& id, std::string& error)
{
    const std::lock_guard lock(mutex);
    sqlite3* const db = Acquire();
    if (db == nullptr)
    {
        error = "The document library could not be opened.";
        return false;
    }
    {
        Transaction transaction(db);
        if (!transaction.IsActive())
        {
            error = "The document library is busy.";
            return false;
        }
        for (const char* sql : {"DELETE FROM document_embeddings WHERE chunk_rowid IN "
                                "(SELECT rowid FROM document_chunks WHERE document_id = ?1);",
                 "DELETE FROM document_chunks WHERE document_id = ?1;", "DELETE FROM documents WHERE id = ?1;"})
        {
            Statement statement = Prepare(db, sql);
            if (!statement)
            {
                error = "The document could not be removed.";
                return false;
            }
            BindText(statement.get(), 1, id);
            if (sqlite3_step(statement.get()) != SQLITE_DONE)
            {
                error = "The document could not be removed.";
                return false;
            }
        }
        if (!transaction.Commit())
        {
            error = "The document could not be removed.";
            return false;
        }
    }
    // As ConversationArchive::ForgetSession does: the deletes only leave tombstones in
    // the search index and old frames in the write-ahead log, so merge the index, then
    // vacuum the freed pages and truncate the log, or the words stay in the files.
    Execute(db, "INSERT INTO document_search(document_search) VALUES('optimize');");
    Execute(db, "VACUUM;");
    Execute(db, "PRAGMA wal_checkpoint(TRUNCATE);");
    return true;
}

} // namespace revia::attachments
