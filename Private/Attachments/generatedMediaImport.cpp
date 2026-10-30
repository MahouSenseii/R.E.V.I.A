#include "Attachments/attachmentLibrary.h"

#include "Audit/contentDigest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::attachments
{
namespace
{
class OwnedGeneratedOutput
{
  public:
    OwnedGeneratedOutput(const std::filesystem::path& path, const std::size_t maximumBytes, const std::string& digest, std::string& error)
        : path(path)
    {
        error = "The verified generated output could not be locked for an exact ownership transfer.";
#ifdef _WIN32
        handle = CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return;
        BY_HANDLE_FILE_INFORMATION information{};
        LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(handle, &information) ||
            (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || information.nNumberOfLinks != 1 ||
            !GetFileSizeEx(handle, &size) || size.QuadPart <= 0 || static_cast<std::uint64_t>(size.QuadPart) > maximumBytes)
            return;
        bytes.resize(static_cast<std::size_t>(size.QuadPart) + 1);
        DWORD received = 0;
        if (!ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &received, nullptr) || received != size.QuadPart)
            return;
        bytes.resize(received);
#else
        std::error_code fileError;
        const auto size = std::filesystem::file_size(path, fileError);
        if (fileError || size == 0 || size > maximumBytes)
            return;
        std::ifstream input(path, std::ios::binary);
        bytes.resize(static_cast<std::size_t>(size) + 1);
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(input.gcount()));
        if (input.bad() || bytes.size() != size)
            return;
#endif
        valid = audit::ContentDigest(bytes) == digest;
        if (valid)
            error.clear();
    }

    ~OwnedGeneratedOutput()
    {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
#endif
    }

    OwnedGeneratedOutput(const OwnedGeneratedOutput&) = delete;
    OwnedGeneratedOutput& operator=(const OwnedGeneratedOutput&) = delete;
    [[nodiscard]] bool IsValid() const
    {
        return valid;
    }
    [[nodiscard]] const std::string& Bytes() const
    {
        return bytes;
    }

    bool Remove(std::string& error)
    {
        error = "The exact verified generated output could not be removed after ownership transfer.";
        if (!valid)
            return false;
#ifdef _WIN32
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)))
            return false;
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
#else
        std::error_code fileError;
        if (!std::filesystem::remove(path, fileError) || fileError)
            return false;
#endif
        valid = false;
        error.clear();
        return true;
    }

  private:
    std::filesystem::path path;
    std::string bytes;
    bool valid = false;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#endif
};

std::string SettingsDigest(const visual::ImageArtifactReceipt& receipt)
{
    return audit::ContentDigest(nlohmann::json{{"provider", receipt.provider}, {"model", receipt.model}, {"pipeline", receipt.pipeline},
        {"variant", receipt.variant}, {"seed", receipt.seed}, {"mode", static_cast<int>(receipt.mode)}, {"width", receipt.width},
        {"height", receipt.height}, {"requestedStrength", receipt.requestedStrength}, {"effectiveStrength", receipt.effectiveStrength},
        {"requestedSteps", receipt.requestedSteps}, {"totalInferenceSteps", receipt.totalInferenceSteps},
        {"denoisingSteps", receipt.denoisingSteps}, {"sourceId", receipt.sourceId}, {"sourceDigest", receipt.sourceDigest},
        {"preparedSourceDigest", receipt.preparedSourceDigest}, {"sourcePreparation", receipt.sourcePreparation}}
            .dump());
}

std::string SettingsDigest(const visual::VideoArtifactReceipt& receipt)
{
    return audit::ContentDigest(nlohmann::json{{"provider", receipt.provider}, {"model", receipt.model}, {"revision", receipt.revision},
        {"pipeline", receipt.pipeline}, {"seed", receipt.seed}, {"mode", static_cast<int>(receipt.mode)}, {"width", receipt.width},
        {"height", receipt.height}, {"frameCount", receipt.frameCount}, {"fps", receipt.fps}, {"durationMs", receipt.durationMs},
        {"steps", receipt.steps}, {"denoisingSteps", receipt.denoisingSteps}, {"device", receipt.device}, {"dtype", receipt.dtype},
        {"offload", receipt.offload}, {"cpuThreads", receipt.cpuThreads}, {"sourceId", receipt.sourceId},
        {"sourceDigest", receipt.sourceDigest}, {"preparedSourceDigest", receipt.preparedSourceDigest},
        {"sourcePreparation", receipt.sourcePreparation}}
            .dump());
}

bool CurrentSource(const DocumentStore& store, const std::string& id, const std::string& digest)
{
    if (id.empty())
        return digest.empty();
    const auto source = store.Find(id);
    return source && source->kind == AttachmentKind::Image && source->digest == digest &&
           ((source->origin == MediaOrigin::UserAttachment && !source->generated) ||
               (source->origin == MediaOrigin::GeneratedArtifact && source->generated));
}
}

GeneratedImageImportResult AttachmentLibrary::ImportGeneratedImage(const visual::ImageResult& artifact,
    const std::filesystem::path& ownedOutputRoot, std::function<bool()> admission, const std::stop_token stopToken)
{
    GeneratedImageImportResult result;
    const auto revision = MediaRevision();
    bool retired = false;
    const auto admitted = [&]
    {
        try
        {
            retired = retired || stopToken.stop_requested() || revision != MediaRevision() || !admission || !admission() ||
                      !CurrentSource(store, artifact.receipt.sourceId, artifact.receipt.sourceDigest);
        }
        catch (...)
        {
            retired = true;
        }
        return !retired;
    };
    const auto& expected = artifact.expectedArtifact;
    const auto& receipt = artifact.receipt;
    result.error = "A generated image requires its exact host-owned output root and verified typed receipt.";
    std::error_code error;
    const auto root = std::filesystem::canonical(ownedOutputRoot, error);
    if (error || ownedOutputRoot.empty() || !std::filesystem::is_directory(root, error) || error || expected.path.empty() ||
        !artifact.succeeded || artifact.cancelled || expected.pipeline.empty() || expected.width <= 0 || expected.height <= 0 ||
        expected.width > static_cast<int>(intake.Limits().maxImageSide) || expected.height > static_cast<int>(intake.Limits().maxImageSide))
        return result;
    const auto path = std::filesystem::canonical(expected.path, error);
    if (error || path.parent_path() != root || std::filesystem::is_symlink(std::filesystem::symlink_status(expected.path, error)) || error)
        return result;
    const auto published = std::filesystem::weakly_canonical(intake.Directory(), error);
    if (error || root == published || path == published)
        return result;
    const auto reported = std::filesystem::canonical(artifact.path, error);
    if (error || reported != path || !visual::VerifyImageArtifact(expected, receipt, result.error))
        return result;
    OwnedGeneratedOutput output(path, intake.Limits().maxImageBytes, receipt.sha256, result.error);
    if (!output.IsValid())
        return result;
    const auto cleanUnpublished = [&](const std::string& reason)
    {
        std::string cleanup;
        result.error = reason;
        if (!output.Remove(cleanup))
            result.error += " " + cleanup;
        else
            result.error += " The unpublished verified output was discarded.";
    };
    if (!admitted())
    {
        cleanUnpublished("The image was generated, but its turn retired before import.");
        return result;
    }
    if (!visual::VerifyImageSource({receipt.jobId, receipt.sha256, "image/png", std::make_shared<const std::string>(output.Bytes()),
                                       static_cast<std::uint32_t>(expected.width), static_cast<std::uint32_t>(expected.height)},
            result.error))
    {
        cleanUnpublished("The generated bytes did not form a supported bounded static PNG.");
        return result;
    }
    auto staged = intake.IntakeBytes("generated-" + receipt.jobId + ".png", output.Bytes(), stopToken);
    if (staged.status != IntakeStatus::Accepted || staged.attachment.kind != AttachmentKind::Image ||
        staged.attachment.mimeType != "image/png" || staged.attachment.digest != receipt.sha256 ||
        staged.attachment.width != static_cast<std::uint32_t>(expected.width) ||
        staged.attachment.height != static_cast<std::uint32_t>(expected.height))
    {
        std::string reason = "The generated image could not be imported into the bounded private image store. " + staged.message;
        if (staged.status == IntakeStatus::Accepted && !intake.Discard(staged.attachment))
            reason += " The new private copy could not be removed.";
        cleanUnpublished(reason);
        return result;
    }
    const auto& attachment = staged.attachment;
    const auto discardPrivateCopy = [&]
    {
        std::error_code remainingError;
        const bool exists = std::filesystem::exists(attachment.storedPath, remainingError);
        return !remainingError && (!exists || intake.Discard(attachment));
    };
    StoredDocument document;
    document.id = attachment.id;
    document.name = attachment.displayName;
    document.kind = attachment.kind;
    document.mimeType = attachment.mimeType;
    document.bytes = attachment.bytes;
    document.digest = attachment.digest;
    document.storedPath = attachment.storedPath;
    document.origin = MediaOrigin::GeneratedArtifact;
    document.generated = GeneratedArtifactProvenance{1, receipt.jobId, receipt.provider, receipt.model,
        receipt.mode == visual::ImageRequestMode::ReferenceToImage ? "reference_to_image" : "text_to_image", receipt.seed,
        SettingsDigest(receipt), receipt.sourceId, receipt.sourceDigest,
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(), {}};
    if (!admitted() || !store.Add(document, {}, result.error))
    {
        std::string reason = result.error.empty() ? "The image was generated, but its turn retired before library commit." : result.error;
        if (!discardPrivateCopy())
            reason += " The new private copy could not be removed.";
        cleanUnpublished(reason);
        return result;
    }
    const auto rollback = [&]
    {
        std::string rollbackError;
        const bool removed = !store.Find(document.id) || Forget(document.id, rollbackError);
        const bool discarded = discardPrivateCopy();
        if (!removed || !discarded)
            result.error += " Import rollback could not fully retire the new library record/copy: " + rollbackError;
    };
    conversationMediaReference importedReference{
        document.id, document.digest, document.name, document.mimeType, attachment.width, attachment.height};
    importedReference.origin = MediaOrigin::GeneratedArtifact;
    if (!admitted() || !ImageReferencesCurrent({importedReference}) || !output.Remove(result.error))
    {
        if (result.error.empty())
            result.error = "The image was generated, but its turn retired before publication.";
        rollback();
        cleanUnpublished(result.error);
        return result;
    }
    if (!admitted())
    {
        result.error = "The image was generated and transferred, but its turn retired before publication.";
        rollback();
        return result;
    }
    result.reference = std::move(importedReference);
    result.storedPath = document.storedPath;
    result.succeeded = true;
    result.error.clear();
    Audit("generated_imported", document.name, "image", document.bytes, document.digest, "verified owned output transferred");
    return result;
}

GeneratedMediaImportResult AttachmentLibrary::ImportGeneratedVideo(const visual::VideoResult& artifact,
    const std::filesystem::path& ownedOutputRoot, std::function<bool()> admission, const std::stop_token stopToken)
{
    GeneratedMediaImportResult result;
    const auto revision = MediaRevision();
    bool retired = false;
    const auto admitted = [&]
    {
        try
        {
            retired = retired || stopToken.stop_requested() || revision != MediaRevision() || !admission || !admission() ||
                      !CurrentSource(store, artifact.receipt.sourceId, artifact.receipt.sourceDigest);
        }
        catch (...)
        {
            retired = true;
        }
        return !retired;
    };
    const auto& expected = artifact.expectedArtifact;
    const auto& receipt = artifact.receipt;
    result.error = "A generated video requires its exact host-owned output root and verified typed receipt.";
    std::error_code error;
    const auto root = std::filesystem::canonical(ownedOutputRoot, error);
    if (error || ownedOutputRoot.empty() || !std::filesystem::is_directory(root, error) || error || expected.path.empty() ||
        !artifact.succeeded || artifact.cancelled)
        return result;
    const auto path = std::filesystem::canonical(expected.path, error);
    if (error || path.parent_path() != root || std::filesystem::is_symlink(std::filesystem::symlink_status(expected.path, error)) || error)
        return result;
    const auto published = std::filesystem::weakly_canonical(intake.Directory(), error);
    if (error || root == published || path == published)
        return result;
    const auto reported = std::filesystem::canonical(artifact.path, error);
    if (error || reported != path)
        return result;
    bool verified = false;
    if (admitted())
        verified = visual::VerifyVideoArtifact(expected, receipt, videoDecoderSettings, stopToken, admitted, result.error);
    // A retired turn may still own a completed output. Only bounded native verification
    // may admit its exact bytes for deletion; it never restores publication admission.
    if (!verified && !admitted())
        verified = visual::VerifyVideoArtifact(expected, receipt, videoDecoderSettings, {}, [] { return true; }, result.error);
    if (!verified)
        return result;
    OwnedGeneratedOutput output(
        path, std::min<std::uintmax_t>(intake.Limits().video.maxBytes, 64U * 1024U * 1024U), receipt.sha256, result.error);
    if (!output.IsValid())
        return result;
    const auto cleanUnpublished = [&](const std::string& reason)
    {
        std::string cleanup;
        result.error = reason;
        if (!output.Remove(cleanup))
            result.error += " " + cleanup;
        else
            result.error += " The unpublished verified output was discarded.";
    };
    if (!admitted())
    {
        cleanUnpublished("The video was generated, but its turn retired before import.");
        return result;
    }
    auto staged = intake.IntakeBytes("generated-" + receipt.jobId + ".mp4", output.Bytes(), stopToken);
    const auto& attachment = staged.attachment;
    const auto discardPrivateCopy = [&]
    {
        std::error_code remainingError;
        const bool exists = std::filesystem::exists(attachment.storedPath, remainingError);
        return !remainingError && (!exists || intake.Discard(attachment));
    };
    if (staged.status != IntakeStatus::Accepted || attachment.kind != AttachmentKind::Video || attachment.mimeType != "video/mp4" ||
        attachment.digest != receipt.sha256 || attachment.width != 384 || attachment.height != 256 || attachment.video.durationMs != 1960 ||
        attachment.video.hasAudio || !attachment.poster || !attachment.poster->bytes)
    {
        std::string reason = "The generated video could not be imported into the bounded private video store. " + staged.message;
        if (staged.status == IntakeStatus::Accepted && !discardPrivateCopy())
            reason += " The new private copy could not be removed.";
        cleanUnpublished(reason);
        return result;
    }
    StoredDocument document;
    document.id = attachment.id;
    document.name = attachment.displayName;
    document.kind = attachment.kind;
    document.mimeType = attachment.mimeType;
    document.bytes = attachment.bytes;
    document.digest = attachment.digest;
    document.storedPath = attachment.storedPath;
    document.origin = MediaOrigin::GeneratedArtifact;
    document.generated = GeneratedArtifactProvenance{1, receipt.jobId, receipt.provider, receipt.model,
        receipt.mode == visual::VideoRequestMode::ImageToVideo ? "image_to_video" : "text_to_video", receipt.seed, SettingsDigest(receipt),
        receipt.sourceId, receipt.sourceDigest,
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(), receipt.revision};
    if (!admitted() || !store.Add(document, {}, result.error))
    {
        std::string reason = result.error.empty() ? "The video was generated, but its turn retired before library commit." : result.error;
        if (!discardPrivateCopy())
            reason += " The new private copy could not be removed.";
        cleanUnpublished(reason);
        return result;
    }
    const auto rollback = [&]
    {
        std::string rollbackError;
        const bool removed = !store.Find(document.id) || Forget(document.id, rollbackError);
        if (!discardPrivateCopy() || !removed)
            result.error += " Import rollback could not fully retire the new library record/copy: " + rollbackError;
    };
    conversationMediaReference reference{
        document.id, document.digest, document.name, document.mimeType, attachment.width, attachment.height};
    reference.durationMs = attachment.video.durationMs;
    reference.sampleEndMs = reference.durationMs - 1;
    reference.origin = MediaOrigin::GeneratedArtifact;
    if (!admitted() || !ImageReferencesCurrent({reference}) || !output.Remove(result.error))
    {
        if (result.error.empty())
            result.error = "The video was generated, but its turn retired before publication.";
        rollback();
        cleanUnpublished(result.error);
        return result;
    }
    if (!admitted())
    {
        result.error = "The video was generated and transferred, but its turn retired before publication.";
        rollback();
        return result;
    }
    result.reference = std::move(reference);
    result.storedPath = document.storedPath;
    result.poster = attachment.poster;
    result.succeeded = true;
    result.error.clear();
    Audit("generated_imported", document.name, "video", document.bytes, document.digest, "verified owned output transferred");
    return result;
}
}
