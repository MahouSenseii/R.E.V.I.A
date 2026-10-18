#pragma once

#include "Attachments/attachmentIntake.h"
#include "Attachments/attachmentTypes.h"
#include "Attachments/documentStore.h"
#include "Core/conversationMessage.h"
#include "LLM/embeddingTypes.h"

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace revia::attachments
{

// Whether a model endpoint is on this computer. Attachments are only ever sent to one.
[[nodiscard]] bool IsLocalEndpoint(const std::string& host);

// Whether pictures can reach the model this turn, and if not, the reason the user is told.
struct ImagePolicy
{
    bool visionAvailable = false;
    std::string unavailableReason;
};

struct TurnAttachments
{
    // The per-turn reference block (attached files, then retrieved passages). Empty when
    // there is nothing to add.
    std::string reference;
    std::vector<conversationImage> images;
    // Short user-facing notes, such as a picture the model could not see.
    std::vector<std::string> notices;
};

// The private attachment library of one companion: intake, the staged files waiting to
// be sent, the stored documents and their search index, and the audit trail.
//
// Every entry point here is for the private audience only. The session checks the
// audience before calling; nothing in this class can be reached from a shared or public
// turn, and nothing here sends a file anywhere but the local model.
class AttachmentLibrary
{
public:
    // query = true asks for a search-query embedding, false for a document one.
    using Embedder = std::function<embeddingOutput(const std::string& text, bool query, std::stop_token stopToken)>;

    AttachmentLibrary(std::filesystem::path root, std::filesystem::path databasePath, std::filesystem::path auditPath,
        AttachmentLimits limits = {});
    ~AttachmentLibrary();
    AttachmentLibrary(const AttachmentLibrary&) = delete;
    AttachmentLibrary& operator=(const AttachmentLibrary&) = delete;

    // Without an embedder (or with one that fails) search is lexical only.
    void SetEmbedder(Embedder embedder);

    IntakeResult Stage(const std::filesystem::path& source);
    IntakeResult StageBytes(const std::string& name, const std::string& bytes);
    // Drops a staged file the user removed before sending; its stored copy is deleted.
    bool Unstage(const std::string& id);
    // Hands the named staged files to a turn. Unknown ids are skipped.
    [[nodiscard]] std::vector<StagedAttachment> TakeStaged(const std::vector<std::string>& ids);
    // Deletes the copies of files taken for a turn that was then refused.
    void DiscardTaken(const std::vector<StagedAttachment>& attachments, const std::string& reason);
    [[nodiscard]] std::vector<StagedAttachment> Staged() const;

    // Files attached to this message: kept in the library, then shown to the model --
    // short documents whole, long ones by their most relevant passages, pictures as
    // image parts when the model can see them.
    [[nodiscard]] TurnAttachments PrepareTurn(const std::string& input, std::vector<StagedAttachment> attachments, const ImagePolicy& images,
        std::stop_token stopToken = {});

    // Passages from earlier attachments for a message that asks about them. Empty when
    // nothing matched or the library is empty.
    [[nodiscard]] std::string SearchReference(const std::string& query, const std::vector<std::string>& restrictTo = {},
        std::stop_token stopToken = {});
    [[nodiscard]] std::vector<DocumentHit> Search(const std::string& query, std::size_t maxHits,
        const std::vector<std::string>& restrictTo = {}, std::stop_token stopToken = {});

    [[nodiscard]] std::vector<StoredDocument> List() const;
    // The start of a stored document's text, for the Documents list.
    [[nodiscard]] std::string PreviewText(const std::string& id, std::size_t maxBytes = 4000) const;
    // Removes a stored file, its passages and their vectors, and records that it did.
    bool Forget(const std::string& id, std::string& error);
    [[nodiscard]] bool HasDocuments() const;
    [[nodiscard]] std::vector<std::string> DocumentNames() const;

    // Whether a message is about the user's documents: it says so, or names one.
    [[nodiscard]] static bool MentionsDocuments(const std::string& input, const std::vector<std::string>& documentNames);

    [[nodiscard]] const AttachmentLimits& Limits() const { return intake.Limits(); }
    [[nodiscard]] const std::filesystem::path& FilesDirectory() const { return intake.Directory(); }

private:
    void Audit(const std::string& event, const std::string& name, const std::string& kind, std::uintmax_t bytes,
        const std::string& digest, const std::string& detail = {});
    bool Commit(const StagedAttachment& attachment, std::size_t& chunkCount, std::string& error);
    void RemoveOrphans();
    void EmbedPending(std::stop_token stopToken);
    void WakeEmbedder();

    AttachmentIntake intake;
    DocumentStore store;
    std::filesystem::path auditPath;
    mutable std::mutex mutex;
    std::map<std::string, StagedAttachment> staged;
    std::mutex auditMutex;
    mutable std::mutex embedderMutex;
    Embedder embedder;
    std::string embeddingModel;
    std::condition_variable_any embedderWake;
    bool embedderPending = false;
    std::jthread embedderWorker;
};

// The executor for a search_documents tool: passages from the user's kept documents that
// match `query`, as a cited reference block, or one plain sentence saying why there are
// none. It never throws. Offer it on private-audience turns only; ReviaSession::
// SearchDocuments applies that check.
[[nodiscard]] std::string ExecuteDocumentSearch(AttachmentLibrary* library, const std::string& query, std::stop_token stopToken = {});

} // namespace revia::attachments
