#include "Attachments/attachmentLibrary.h"

#include "Actions/actionTypes.h"
#include "Attachments/attachmentPrompt.h"
#include "Attachments/documentChunker.h"
#include "Core/utf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <set>
#include <system_error>

namespace revia::attachments
{
namespace
{
constexpr std::size_t PassagesPerLongDocument = 3;
constexpr std::size_t PassagesPerSearch = 4;

std::string Lowered(std::string_view text)
{
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return lowered;
}

std::int64_t EpochSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string ReadBounded(const std::filesystem::path& path, const std::uintmax_t limit)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > limit)
        return {};
    std::ifstream file(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<std::uintmax_t>(file.gcount()) == size ? bytes : std::string();
}
} // namespace

bool IsLocalEndpoint(const std::string& host)
{
    const std::string lowered = Lowered(host);
    if (lowered == "localhost" || lowered == "::1" || lowered == "[::1]")
        return true;
    // A loopback address written out, not a name that merely starts with 127. (a name
    // such as 127.0.0.1.example.com resolves wherever its owner likes).
    return lowered.rfind("127.", 0) == 0 && std::count(lowered.begin(), lowered.end(), '.') == 3 &&
           std::all_of(lowered.begin(), lowered.end(), [](const char character) { return character == '.' || std::isdigit(static_cast<unsigned char>(character)); });
}

AttachmentLibrary::AttachmentLibrary(std::filesystem::path root, std::filesystem::path databasePath, std::filesystem::path inputAuditPath,
    AttachmentLimits limits)
    : intake(std::move(root), limits), store(std::move(databasePath)), auditPath(std::move(inputAuditPath))
{
    RemoveOrphans();
    embedderWorker = std::jthread([this](const std::stop_token stopToken) { EmbedPending(stopToken); });
}

AttachmentLibrary::~AttachmentLibrary()
{
    embedderWorker.request_stop();
    embedderWake.notify_all();
    if (embedderWorker.joinable())
        embedderWorker.join();
    // Files staged and never sent do not outlive the session.
    std::lock_guard lock(mutex);
    for (const auto& [id, attachment] : staged)
        intake.Discard(attachment);
}

void AttachmentLibrary::SetEmbedder(Embedder value)
{
    {
        std::lock_guard lock(embedderMutex);
        embedder = std::move(value);
    }
    WakeEmbedder();
}

void AttachmentLibrary::WakeEmbedder()
{
    {
        std::lock_guard lock(embedderMutex);
        embedderPending = true;
    }
    embedderWake.notify_all();
}

void AttachmentLibrary::EmbedPending(const std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        Embedder embed;
        {
            std::unique_lock lock(embedderMutex);
            if (!embedderWake.wait(lock, stopToken, [this] { return embedderPending; }))
                return;
            embedderPending = false;
            embed = embedder;
        }
        if (!embed)
            continue;
        // Learns the model name from the first successful embedding, so vectors are
        // always recorded under the model that really produced them.
        for (bool progress = true; progress && !stopToken.stop_requested();)
        {
            progress = false;
            std::string model;
            {
                std::lock_guard lock(embedderMutex);
                model = embeddingModel;
            }
            // Until the model is known, any one chunk teaches it.
            std::vector<PendingEmbedding> pending =
                model.empty() ? store.MissingEmbeddings("unknown model", 1) : store.MissingEmbeddings(model, 8);
            for (const PendingEmbedding& chunk : pending)
            {
                const embeddingOutput output = embed(chunk.text, false, stopToken);
                if (!output.bSuccess || output.model.empty())
                    break;
                {
                    std::lock_guard lock(embedderMutex);
                    embeddingModel = output.model;
                }
                progress = store.SetEmbedding(chunk.chunkRowId, output.values, output.model) || progress;
            }
        }
    }
}

void AttachmentLibrary::Audit(const std::string& event, const std::string& name, const std::string& kind, const std::uintmax_t bytes,
    const std::string& digest, const std::string& detail)
{
    // Who, what and which bytes -- never the content.
    nlohmann::json record = {{"time", EpochSeconds()}, {"event", event}, {"name", name}, {"kind", kind}, {"bytes", bytes}};
    if (!digest.empty())
        record["sha256"] = digest;
    if (!detail.empty())
        record["detail"] = detail;
    std::lock_guard lock(auditMutex);
    std::error_code error;
    std::filesystem::create_directories(auditPath.parent_path(), error);
    std::ofstream file(auditPath, std::ios::binary | std::ios::app);
    file << record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
}

IntakeResult AttachmentLibrary::Stage(const std::filesystem::path& source)
{
    IntakeResult result = intake.IntakeFile(source);
    const std::string name = SanitizeDisplayName(revia::actions::PathToUtf8(source.filename()));
    if (result.status != IntakeStatus::Accepted)
    {
        Audit(result.status == IntakeStatus::Withheld ? "withheld" : "refused", name, {}, 0, {}, result.message);
        return result;
    }
    const StagedAttachment& accepted = result.attachment;
    Audit("staged", accepted.displayName, ToString(accepted.kind), accepted.bytes, accepted.digest);
    std::lock_guard lock(mutex);
    staged[accepted.id] = accepted;
    return result;
}

IntakeResult AttachmentLibrary::StageBytes(const std::string& name, const std::string& bytes)
{
    IntakeResult result = intake.IntakeBytes(name, bytes);
    if (result.status != IntakeStatus::Accepted)
    {
        Audit(result.status == IntakeStatus::Withheld ? "withheld" : "refused", SanitizeDisplayName(name), {}, bytes.size(), {},
            result.message);
        return result;
    }
    const StagedAttachment& accepted = result.attachment;
    Audit("staged", accepted.displayName, ToString(accepted.kind), accepted.bytes, accepted.digest);
    std::lock_guard lock(mutex);
    staged[accepted.id] = accepted;
    return result;
}

bool AttachmentLibrary::Unstage(const std::string& id)
{
    StagedAttachment removed;
    {
        std::lock_guard lock(mutex);
        const auto found = staged.find(id);
        if (found == staged.end())
            return false;
        removed = std::move(found->second);
        staged.erase(found);
    }
    intake.Discard(removed);
    Audit("removed", removed.displayName, ToString(removed.kind), removed.bytes, removed.digest);
    return true;
}

std::vector<StagedAttachment> AttachmentLibrary::TakeStaged(const std::vector<std::string>& ids)
{
    std::vector<StagedAttachment> taken;
    std::lock_guard lock(mutex);
    for (const std::string& id : ids)
    {
        const auto found = staged.find(id);
        if (found == staged.end())
            continue;
        taken.push_back(std::move(found->second));
        staged.erase(found);
    }
    return taken;
}

void AttachmentLibrary::DiscardTaken(const std::vector<StagedAttachment>& attachments, const std::string& reason)
{
    for (const StagedAttachment& attachment : attachments)
    {
        intake.Discard(attachment);
        Audit("discarded", attachment.displayName, ToString(attachment.kind), attachment.bytes, attachment.digest, reason);
    }
}

std::vector<StagedAttachment> AttachmentLibrary::Staged() const
{
    std::vector<StagedAttachment> list;
    std::lock_guard lock(mutex);
    for (const auto& [id, attachment] : staged)
        list.push_back(attachment);
    return list;
}

bool AttachmentLibrary::Commit(const StagedAttachment& attachment, std::size_t& chunkCount, std::string& error)
{
    StoredDocument document;
    document.id = attachment.id;
    document.name = attachment.displayName;
    document.kind = attachment.kind;
    document.mimeType = attachment.mimeType;
    document.bytes = attachment.bytes;
    document.digest = attachment.digest;
    document.pages = attachment.pageOffsets.size();
    document.storedPath = attachment.storedPath;
    const std::vector<DocumentChunk> chunks =
        IsDocument(attachment.kind) ? ChunkDocument(attachment.text, attachment.pageOffsets) : std::vector<DocumentChunk>{};
    chunkCount = chunks.size();
    if (!store.Add(document, chunks, error))
        return false;
    if (!chunks.empty())
        WakeEmbedder();
    return true;
}

TurnAttachments AttachmentLibrary::PrepareTurn(const std::string& input, std::vector<StagedAttachment> attachments, const ImagePolicy& images,
    const std::stop_token stopToken)
{
    TurnAttachments turn;
    const AttachmentLimits& limits = intake.Limits();
    if (attachments.size() > limits.maxAttachmentsPerTurn)
    {
        std::vector<StagedAttachment> extra(attachments.begin() + static_cast<std::ptrdiff_t>(limits.maxAttachmentsPerTurn), attachments.end());
        attachments.resize(limits.maxAttachmentsPerTurn);
        DiscardTaken(extra, "more than " + std::to_string(limits.maxAttachmentsPerTurn) + " files in one message");
        turn.notices.push_back("Only the first " + std::to_string(limits.maxAttachmentsPerTurn) + " files were sent with this message.");
    }
    std::vector<AttachedFileView> views;
    for (const StagedAttachment& attachment : attachments)
    {
        std::string error;
        std::size_t chunkCount = 0;
        if (!Commit(attachment, chunkCount, error))
        {
            intake.Discard(attachment);
            turn.notices.push_back("\"" + attachment.displayName + "\" could not be kept: " + error);
            continue;
        }
        Audit("sent", attachment.displayName, ToString(attachment.kind), attachment.bytes, attachment.digest);

        AttachedFileView view;
        view.name = attachment.displayName;
        view.kind = attachment.kind;
        view.bytes = attachment.bytes;
        view.pages = attachment.pageOffsets.size();
        view.note = attachment.note;
        if (attachment.kind == AttachmentKind::Image)
        {
            view.width = attachment.width;
            view.height = attachment.height;
            if (!images.visionAvailable)
            {
                view.imageUnavailableReason = images.unavailableReason;
            }
            else if (turn.images.size() >= limits.maxImagesPerTurn)
            {
                view.imageUnavailableReason = "only " + std::to_string(limits.maxImagesPerTurn) + " pictures can be sent with one message.";
            }
            else if (std::string bytes = ReadBounded(attachment.storedPath, limits.maxImageBytes); bytes.empty())
            {
                view.imageUnavailableReason = "its stored copy could not be read.";
            }
            else
            {
                turn.images.push_back({attachment.displayName, attachment.mimeType, std::make_shared<const std::string>(std::move(bytes)),
                    attachment.width, attachment.height});
                view.imageVisible = true;
            }
            if (!view.imageVisible)
                turn.notices.push_back("Revia could not see \"" + attachment.displayName + "\": " + view.imageUnavailableReason);
        }
        else if (attachment.text.size() <= limits.inlineTextBytes)
        {
            view.wholeText = attachment.text;
            view.pageOffsets = attachment.pageOffsets;
        }
        else
        {
            view.totalChunks = chunkCount;
            view.passages = Search(input, PassagesPerLongDocument, {attachment.id}, stopToken);
            if (view.passages.empty())
                if (auto opening = store.ChunkAt(attachment.id, 0))
                    view.passages.push_back(std::move(*opening));
        }
        views.push_back(std::move(view));
    }
    turn.reference = BuildAttachmentBlock(views);
    return turn;
}

std::vector<DocumentHit> AttachmentLibrary::Search(
    const std::string& query, const std::size_t maxHits, const std::vector<std::string>& restrictTo, const std::stop_token stopToken)
{
    Embedder embed;
    {
        std::lock_guard lock(embedderMutex);
        embed = embedder;
    }
    embeddingOutput queryEmbedding;
    if (embed && !query.empty())
        queryEmbedding = embed(query, true, stopToken);
    return store.Search(query, queryEmbedding.bSuccess ? queryEmbedding.values : std::vector<float>{},
        queryEmbedding.bSuccess ? queryEmbedding.model : std::string(), maxHits, restrictTo);
}

std::string AttachmentLibrary::SearchReference(const std::string& query, const std::vector<std::string>& restrictTo, const std::stop_token stopToken)
{
    if (!store.HasDocuments())
        return {};
    return BuildDocumentSearchBlock(Search(query, PassagesPerSearch, restrictTo, stopToken));
}

std::vector<StoredDocument> AttachmentLibrary::List() const
{
    std::string model;
    {
        std::lock_guard lock(embedderMutex);
        model = embeddingModel;
    }
    return store.List(model);
}

std::string AttachmentLibrary::PreviewText(const std::string& id, const std::size_t maxBytes) const
{
    const auto opening = store.ChunkAt(id, 0);
    if (!opening)
        return {};
    std::string text = opening->text.size() > maxBytes ? revia::utf8::Prefix(opening->text, maxBytes) + " [...]" : opening->text;
    if (store.ChunkAt(id, 1))
        text += "\n\n[The document continues.]";
    return text;
}

bool AttachmentLibrary::Forget(const std::string& id, std::string& error)
{
    const std::optional<StoredDocument> document = store.Find(id);
    if (!document)
    {
        error = "That document is not in the library.";
        return false;
    }
    if (!store.Forget(id, error))
        return false;
    std::error_code removeError;
    if (document->storedPath.parent_path() == intake.Directory())
        std::filesystem::remove(document->storedPath, removeError);
    Audit("forgotten", document->name, ToString(document->kind), document->bytes, document->digest);
    return true;
}

bool AttachmentLibrary::HasDocuments() const
{
    return store.HasDocuments();
}

std::vector<std::string> AttachmentLibrary::DocumentNames() const
{
    std::vector<std::string> names;
    for (const StoredDocument& document : store.List())
        if (IsDocument(document.kind))
            names.push_back(document.name);
    return names;
}

bool AttachmentLibrary::MentionsDocuments(const std::string& input, const std::vector<std::string>& documentNames)
{
    static const std::set<std::string> Words = {"document", "documents", "doc", "docs", "docx", "file", "files", "pdf", "pdfs", "attachment",
        "attachments", "attached", "upload", "uploaded", "spreadsheet", "csv"};
    const std::string lowered = Lowered(input);
    std::string word;
    for (std::size_t index = 0; index <= lowered.size(); ++index)
    {
        const char character = index < lowered.size() ? lowered[index] : ' ';
        if (std::isalnum(static_cast<unsigned char>(character)))
        {
            word.push_back(character);
            continue;
        }
        if (Words.contains(word))
            return true;
        word.clear();
    }
    for (const std::string& name : documentNames)
    {
        const std::string full = Lowered(name);
        const std::size_t dot = full.find_last_of('.');
        const std::string stem = dot == std::string::npos ? full : full.substr(0, dot);
        if ((full.size() >= 4 && lowered.find(full) != std::string::npos) || (stem.size() >= 4 && lowered.find(stem) != std::string::npos))
            return true;
    }
    return false;
}

std::string ExecuteDocumentSearch(AttachmentLibrary* library, const std::string& query, const std::stop_token stopToken)
{
    try
    {
        if (query.find_first_not_of(" \t\n") == std::string::npos)
            return "No search was run: the query was empty.";
        if (library == nullptr || !library->HasDocuments())
            return "The user has not attached any documents yet.";
        const std::string found = library->SearchReference(query, {}, stopToken);
        return found.empty() ? "No passage in the user's documents matched that search." : found;
    }
    catch (const std::exception&)
    {
        return "The document search failed.";
    }
}

void AttachmentLibrary::RemoveOrphans()
{
    // A file staged and never sent, or a copy left by a crash, is not in the library and
    // should not stay on disk.
    std::set<std::filesystem::path> kept;
    for (const StoredDocument& document : store.List())
        kept.insert(document.storedPath.lexically_normal());
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(intake.Directory(), error))
    {
        if (!entry.is_regular_file(error) || kept.contains(entry.path().lexically_normal()))
            continue;
        std::filesystem::remove(entry.path(), error);
    }
}

} // namespace revia::attachments
