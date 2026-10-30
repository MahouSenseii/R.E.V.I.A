#pragma once

#include "Attachments/attachmentTypes.h"
#include "Attachments/documentChunker.h"
#include "Core/conversationMediaReference.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace revia::attachments
{

struct GeneratedArtifactProvenance
{
    std::uint32_t version = 1;
    std::string jobId;
    std::string provider;
    std::string model;
    std::string mode;
    std::uint32_t seed = 0;
    std::string settingsDigest;
    std::string sourceId;
    std::string sourceDigest;
    std::int64_t createdAt = 0;
    std::string modelRevision;
    bool operator==(const GeneratedArtifactProvenance&) const = default;
};

// One kept file's record, including its bounded origin metadata rather than content.
struct StoredDocument
{
    std::string id;
    std::string name;
    AttachmentKind kind = AttachmentKind::Text;
    std::string mimeType;
    std::uintmax_t bytes = 0;
    std::string digest;
    std::size_t pages = 0;
    std::size_t chunks = 0;
    std::size_t embeddedChunks = 0;
    std::filesystem::path storedPath;
    std::int64_t addedAt = 0;
    MediaOrigin origin = MediaOrigin::UserAttachment;
    std::optional<GeneratedArtifactProvenance> generated;
};

struct DocumentHit
{
    std::string documentId;
    std::string documentName;
    std::size_t chunkIndex = 0;
    std::size_t page = 0;
    std::string text;
    double score = 0.0;
};

struct PendingEmbedding
{
    std::int64_t chunkRowId = 0;
    std::string text;
};

// The companion's private library of attached files, in its own SQLite database. Chunks
// are ranked the way long-term memory is: FTS5 bm25 and embedding cosine, fused by
// reciprocal rank (Memory/hybridRanking.h). Only private-audience turns ever read it.
class DocumentStore
{
public:
    explicit DocumentStore(std::filesystem::path databasePath);
    ~DocumentStore();
    DocumentStore(const DocumentStore&) = delete;
    DocumentStore& operator=(const DocumentStore&) = delete;

    bool Add(const StoredDocument& document, const std::vector<DocumentChunk>& chunks, std::string& error);
    // Chunks of `documentId` (or of every document when empty) with no vector for `model`.
    [[nodiscard]] std::vector<PendingEmbedding> MissingEmbeddings(const std::string& model, std::size_t limit,
        const std::string& documentId = {}) const;
    bool SetEmbedding(std::int64_t chunkRowId, const std::vector<float>& vector, const std::string& model);

    // Best chunks for the query. `restrictTo` limits the search to those documents.
    [[nodiscard]] std::vector<DocumentHit> Search(const std::string& query, const std::vector<float>& queryEmbedding,
        const std::string& embeddingModel, std::size_t maxHits, const std::vector<std::string>& restrictTo = {}) const;
    [[nodiscard]] std::optional<DocumentHit> ChunkAt(const std::string& documentId, std::size_t chunkIndex) const;

    [[nodiscard]] std::vector<StoredDocument> List(const std::string& embeddingModel = {}) const;
    [[nodiscard]] std::optional<StoredDocument> Find(const std::string& id) const;
    [[nodiscard]] bool HasDocuments() const;

    // Deletes the record, its chunks, their search index entries and vectors, then
    // optimizes the index and vacuums the file so the text is not recoverable from it.
    // The stored copy of the file is the caller's to delete.
    bool Forget(const std::string& id, std::string& error);

    [[nodiscard]] const std::filesystem::path& Path() const { return path; }

private:
    sqlite3* Acquire() const;

    std::filesystem::path path;
    mutable std::mutex mutex;
    mutable sqlite3* database = nullptr;
};

} // namespace revia::attachments
