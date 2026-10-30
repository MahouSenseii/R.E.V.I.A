#include "Attachments/attachmentLibrary.h"
#include "Attachments/mediaSelection.h"
#include "Core/conversationContext.h"
#include "Memory/conversationArchive.h"
#include "Visual/imageTypes.h"
#include "Audit/contentDigest.h"
#include "testSupport.h"

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace
{
void Check(const bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

revia::memory::MemoryScope PrivateScope()
{
    revia::memory::MemoryScope scope;
    scope.companionId = "generated-fixture-companion";
    scope.participantId = "generated-fixture-person";
    scope.participantSource = revia::identity::SpeakerSource::ExplicitIntroduction;
    scope.audience = {revia::identity::AudienceKind::Private, "generated-fixture-room", 7, {}};
    return scope;
}

std::string TinyPng()
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path() / "Fixtures/Attachments/tiny.png", std::ios::binary);
    Check(static_cast<bool>(input), "The generated continuity fixture PNG is unavailable.");
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void Sql(const std::filesystem::path& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    Check(sqlite3_open(revia::actions::PathToUtf8(path).c_str(), &database) == SQLITE_OK, "Fixture SQLite open failed.");
    const int result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(database);
    Check(result == SQLITE_OK, "Fixture SQLite statement failed.");
}

conversationMediaReference GeneratedReference(const std::string& id, const char digest)
{
    conversationMediaReference reference{id, std::string(64, digest), id + ".png", "image/png", 4, 3};
    reference.origin = MediaOrigin::GeneratedArtifact;
    return reference;
}

void TestStoredGeneratedOriginPersistsAndLegacyRowsMigrate()
{
    using namespace revia::attachments;
    revia::tests::ScopedTestDirectory directory;
    const auto path = directory.root / "documents.db";
    Sql(path,
        "CREATE TABLE documents(id TEXT PRIMARY KEY,name TEXT NOT NULL,kind TEXT NOT NULL,mime TEXT NOT NULL,bytes INTEGER NOT "
        "NULL,digest TEXT NOT NULL,pages INTEGER NOT NULL,chunk_count INTEGER NOT NULL,stored_path TEXT NOT NULL,added_at INTEGER "
        "NOT NULL); INSERT INTO documents VALUES('legacy','legacy.png','image','image/png',12,'legacy-digest',0,0,'legacy.png',123);");
    StoredDocument generated;
    generated.id = "generated";
    generated.name = "generated.png";
    generated.kind = AttachmentKind::Image;
    generated.mimeType = "image/png";
    generated.bytes = TinyPng().size();
    generated.digest = std::string(64, 'a');
    generated.storedPath = directory.root / "generated.png";
    generated.origin = MediaOrigin::GeneratedArtifact;
    generated.generated = GeneratedArtifactProvenance{
        1, "job-one", "local_diffusers", "sd-turbo", "text_to_image", 42, std::string(64, 'b'), {}, {}, 1234, {}};
    {
        DocumentStore store(path);
        const auto legacy = store.Find("legacy");
        Check(legacy && legacy->origin == MediaOrigin::UserAttachment && !legacy->generated && legacy->addedAt == 123 &&
                  legacy->digest == "legacy-digest",
            "Legacy document migration changed an existing upload row.");
        std::string error;
        Check(store.Add(generated, {}, error), "A valid generated record was refused: " + error);
    }
    {
        DocumentStore reopened(path);
        const auto restored = reopened.Find("generated");
        Check(restored && restored->origin == MediaOrigin::GeneratedArtifact && restored->generated == generated.generated,
            "Generated origin and bounded provenance did not survive SQLite reopen.");
        auto invalid = generated;
        invalid.id = "invalid";
        invalid.origin = static_cast<MediaOrigin>(99);
        std::string error;
        Check(!reopened.Add(invalid, {}, error), "An unknown stored media origin was committed.");
        invalid.origin = MediaOrigin::UserAttachment;
        Check(!reopened.Add(invalid, {}, error), "Upload provenance accepted generated-only metadata.");
    }
}

void TestGeneratedAssistantMetadataKeepsScopedVersionSelection()
{
    const auto scope = PrivateScope();
    conversationMediaReference uploaded{"upload", std::string(64, 'c'), "upload.png", "image/png", 4, 3};
    const auto first = GeneratedReference("generated-one", 'a');
    const auto second = GeneratedReference("generated-two", 'b');
    conversationMessage upload{"user", "Use this uploaded reference.", scope.participantId, scope};
    upload.mediaReferences = {uploaded};
    conversationMessage generated{"assistant", "A generated image is ready.", scope.participantId, scope};
    generated.mediaReferences = {first};
    conversationMessage newer = generated;
    newer.content = "The generated variation must retain generated_output_analysis_fixture.";
    newer.mediaReferences = {second};
    conversationContext context;
    context.AddMessage(upload);
    context.AddMessage(generated);
    context.AddMessage(newer);
    conversationContext ownContent;
    ownContent.AddMessage(newer);
    ownContent.InvalidateMediaAnalysis(second.id);
    Check(ownContent.GetRecentMessages().front().content != newer.content &&
              ownContent.GetRecentMessages().front().mediaReferences == std::vector{second},
        "Forget did not clear own generated content while retaining unavailable source identity.");
    Check(context.GetMediaHistory().size() == 3 && context.GetMediaHistory().back().role == "assistant" &&
              context.GetMediaHistory().back().images.empty(),
        "Host-generated assistant associations were lost or retained pixel history.");
    using revia::attachments::SelectConversationImages;
    const auto history = context.GetMediaHistory();
    Check(SelectConversationImages("Describe it.", history, scope).references == std::vector{second},
        "A generated result did not become the current scoped source.");
    Check(SelectConversationImages("Compare it with the previous version.", history, scope).references == std::vector{first, second},
        "Generated versions lost chronological comparison identity.");
    Check(SelectConversationImages("Describe the uploaded image.", history, scope).references == std::vector{uploaded},
        "A generated artifact was misrepresented as a user-uploaded image.");
    Check(SelectConversationImages("Describe the generated image.", history, scope).references == std::vector{second},
        "An explicit generated-image reference selected an upload.");
    Check(SelectConversationImages("Compare it with the uploaded image.", history, scope).references == std::vector{uploaded, second},
        "A cross-origin comparison dropped the current generated result or its uploaded source.");
    Check(
        SelectConversationImages("Compare the generated and uploaded images.", history, scope).references == std::vector{uploaded, second},
        "An explicit upload/generated comparison selected two generated versions.");
    Check(SelectConversationImages("Read the word \"generated\" from the uploaded image.", history, scope).references ==
              std::vector{uploaded},
        "Quoted image text was interpreted as generated-origin selection.");
    const auto contradicted = SelectConversationImages("Inspect the uploaded image generated-two.png.", history, scope);
    Check(contradicted.references.empty() && !contradicted.notice.empty(),
        "A named generated image with a conflicting uploaded qualifier silently selected another image.");
    auto forged = generated;
    forged.mediaReferences = {uploaded};
    conversationContext rejected;
    rejected.AddMessage(forged);
    Check(rejected.GetMediaHistory().empty() && SelectConversationImages("Describe the image.", {forged}, scope).references.empty(),
        "A generic assistant upload association was promoted into source history.");
    auto foreign = scope;
    foreign.participantId = "other-person";
    Check(SelectConversationImages("Describe the generated image.", history, foreign).references.empty(),
        "A generated artifact escaped its participant scope.");
    for (int index = 0; index < 70; ++index)
        context.AddMessage(index % 2 == 0 ? "user" : "assistant", "Unrelated background exchange " + std::to_string(index));
    Check(SelectConversationImages("Compare the previous image with the last image.", context.GetMediaHistory(), scope).references ==
              std::vector{first, second},
        "Generated metadata was lost when text history was trimmed.");
    context.InvalidateMediaAnalysis(second.id);
    const auto afterForget = context.GetMediaHistory();
    Check(SelectConversationImages("Describe the generated image.", afterForget, scope).references == std::vector{second},
        "Forget discarded unavailable identity and silently selected an older generated version.");
    const auto recent = context.GetRecentMessages();
    Check(std::none_of(recent.begin(), recent.end(), [&](const auto& message) { return message.content == newer.content; }),
        "Forget retained the generated artifact's own assistant content.");
    Check(context.GetCompressedHistorySummary().find("generated_output_analysis_fixture") == std::string::npos,
        "Forget retained evicted generated-artifact content in working continuity.");
    context.Clear();
    Check(context.GetMediaHistory().empty(), "A new chat retained generated associations.");
}

void TestDedicatedGeneratedAssistantArchiveKeepsGenericGuardAndScope()
{
    revia::tests::ScopedTestDirectory directory;
    const auto scope = PrivateScope();
    const auto generated = GeneratedReference("generated-archive", 'd');
    const auto path = directory.root / "archive.db";
    revia::memory::ConversationArchive archive(revia::actions::PathToUtf8(path));
    std::string reason;
    Check(archive.BeginSession("own-chat", reason), reason);
    Check(!archive.Record("own-chat", "assistant", "Untrusted generic assistant metadata.", reason, scope, {}, {generated}),
        "Generic assistant Record accepted generated media.");
    Check(archive.RecordGeneratedAssistant(
              "own-chat", "The generated image must preserve archive_generated_fixture.", {generated}, reason, scope, [] { return true; }),
        "The admitted host generated-assistant path was refused: " + reason);
    auto restored = archive.LoadSessionTail("own-chat", 20, scope);
    Check(restored.size() == 1 && restored.front().role == "assistant" && restored.front().mediaReferences == std::vector{generated},
        "Generated assistant origin did not survive archive restore.");
    auto foreign = scope;
    foreign.participantId = "other";
    Check(archive.LoadSessionTail("own-chat", 20, foreign).empty(), "Generated archive metadata escaped its private participant.");
    Check(!archive.RecordGeneratedAssistant("own-chat", "Expired output.", {generated}, reason, scope, [] { return false; }),
        "Retired archive admission stored a generated association.");
    auto uploaded = generated;
    uploaded.origin = MediaOrigin::UserAttachment;
    Check(!archive.RecordGeneratedAssistant("own-chat", "Forged uploaded result.", {uploaded}, reason, scope, [] { return true; }),
        "The generated-assistant API accepted upload origin.");
    auto shared = scope;
    shared.audience.kind = revia::identity::AudienceKind::Shared;
    Check(!archive.RecordGeneratedAssistant("own-chat", "Shared result.", {generated}, reason, shared, [] { return true; }),
        "Generated artifact metadata was durably published to a shared scope.");
    Check(archive.InvalidateMediaAnalysis(generated.id), "Archive generated invalidation failed.");
    restored = archive.LoadSessionTail("own-chat", 20, scope);
    Check(restored.front().content.find("archive_generated_fixture") == std::string::npos &&
              restored.front().mediaReferences == std::vector{generated},
        "Forget retained generated assistant content or lost its unavailable identity tombstone.");
    Check(archive.Record("own-chat", "assistant", "Legacy assistant.", reason, scope), "Legacy assistant fixture failed.");
    const auto legacy = nlohmann::json::array(
        {{{"id", "legacy-image"}, {"digest", std::string(64, 'e')}, {"name", "legacy.png"}, {"mimeType", "image/png"}, {"width", 4},
            {"height", 3}}}).dump();
    Sql(path, "UPDATE conversation_turns SET media_references='" + legacy + "' WHERE content='Legacy assistant.';");
    Check(archive.LoadSessionTail("own-chat", 20, scope).back().mediaReferences.empty(),
        "Legacy generic assistant metadata was promoted into a generated source.");
    const auto duplicate = legacy.substr(0, legacy.size() - 2) + ",\"origin\":\"user_attachment\",\"origin\":\"generated_artifact\"}]";
    Sql(path, "UPDATE conversation_turns SET media_references='" + duplicate + "' WHERE content='Legacy assistant.';");
    Check(archive.LoadSessionTail("own-chat", 20, scope).back().mediaReferences.empty(),
        "Duplicate origin keys promoted ambiguous assistant metadata into a generated source.");
}

void TestImageSourceReadIgnoresVisionButAuthenticatesCurrentOrigin()
{
    using namespace revia::attachments;
    revia::tests::ScopedTestDirectory directory;
    AttachmentLibrary library(directory.root / "files", directory.root / "library.db", directory.root / "audit.jsonl");
    const auto staged = library.StageBytes("reference.png", TinyPng());
    Check(staged.status == IntakeStatus::Accepted, "The source PNG was not staged.");
    const auto prepared = library.PrepareTurn("Use this reference.", library.TakeStaged({staged.attachment.id}), {false, "Vision off"});
    Check(prepared.mediaReferences.size() == 1 && prepared.images.empty(), "The fixture did not keep an unseen upload.");
    const auto reference = prepared.mediaReferences.front();
    const auto source = library.ReadImageSource(reference);
    Check(source && source->reference == reference && source->pixels.bytes && *source->pixels.bytes == TinyPng(),
        "A valid stored source required a vision-enabled endpoint or lost its exact bytes.");
    auto forged = reference;
    forged.origin = MediaOrigin::GeneratedArtifact;
    Check(!library.ReadImageSource(forged) && !library.ImageReferencesCurrent({forged}),
        "Serialized generated origin overrode the current upload record.");
    std::stop_source cancelled;
    cancelled.request_stop();
    Check(!library.ReadImageSource(reference, cancelled.get_token()), "A cancelled source read returned pixels.");
    {
        std::ofstream output(staged.attachment.storedPath, std::ios::binary | std::ios::app);
        output << "changed";
    }
    Check(!library.ReadImageSource(reference), "A modified source file was read as the original image.");
    std::string error;
    Check(library.Forget(reference.id, error), error);
    Check(!library.ReadImageSource(reference), "A forgotten source remained available.");
}

revia::visual::ImageResult VerifiedGeneratedFixture(const std::filesystem::path& outputRoot, const std::string& job)
{
    std::filesystem::create_directories(outputRoot);
    revia::visual::ImageResult result;
    result.succeeded = true;
    result.path = outputRoot / (job + ".png");
    {
        std::ofstream output(result.path, std::ios::binary);
        output << TinyPng();
    }
    auto& expected = result.expectedArtifact;
    expected.path = result.path;
    expected.jobId = job;
    expected.model = "sd-turbo";
    expected.width = 4;
    expected.height = 3;
    expected.pipeline = "StableDiffusionPipeline";
    expected.variant = "fp16";
    expected.seed = 42;
    expected.requestedSteps = expected.totalInferenceSteps = expected.denoisingSteps = 4;
    auto& receipt = result.receipt;
    receipt.path = expected.path;
    receipt.jobId = expected.jobId;
    receipt.model = expected.model;
    receipt.width = expected.width;
    receipt.height = expected.height;
    receipt.provider = expected.provider;
    receipt.pipeline = expected.pipeline;
    receipt.variant = expected.variant;
    receipt.seed = expected.seed;
    receipt.requestedSteps = receipt.totalInferenceSteps = receipt.denoisingSteps = 4;
    receipt.sha256 = revia::audit::ContentDigest(TinyPng());
    return result;
}

void TestGeneratedImportTransfersExactlyOneOwnedOutput()
{
    using namespace revia::attachments;
    revia::tests::ScopedTestDirectory directory;
    const auto files = directory.root / "files", database = directory.root / "library.db", audit = directory.root / "audit.jsonl";
    const auto outputRoot = directory.root / "output";
    auto artifact = VerifiedGeneratedFixture(outputRoot, "job-transfer");
    std::string verificationError;
    Check(revia::visual::VerifyImageArtifact(artifact.expectedArtifact, artifact.receipt, verificationError),
        "The generated CPU fixture failed strict native verification: " + verificationError);
    const auto unrelated = outputRoot / "unrelated.png";
    std::filesystem::copy_file(artifact.path, unrelated);
    conversationMediaReference importedReference;
    std::filesystem::path importedPath;
    {
        AttachmentLibrary library(files, database, audit);
        const auto revision = library.MediaRevision();
        const auto imported = library.ImportGeneratedImage(artifact, outputRoot, [] { return true; });
        Check(imported.succeeded && imported.reference.origin == MediaOrigin::GeneratedArtifact &&
                  imported.reference.digest == artifact.receipt.sha256 && imported.storedPath.parent_path() == files &&
                  !std::filesystem::exists(artifact.path) && std::filesystem::exists(imported.storedPath) &&
                  std::filesystem::exists(unrelated),
            "Generated import did not transfer only its exact verified output: " + imported.error);
        Check(library.MediaRevision() == revision, "Import retired the source admission of its own completed turn.");
        importedReference = imported.reference;
        importedPath = imported.storedPath;
        const auto source = library.ReadImageSource(imported.reference);
        Check(source && *source->pixels.bytes == TinyPng(), "An imported generated result could not become an exact follow-up source.");
        auto forgedUpload = imported.reference;
        forgedUpload.origin = MediaOrigin::UserAttachment;
        Check(!library.ReadImageSource(forgedUpload), "A generated store record was authenticated as an uploaded image.");
        Check(library.Staged().empty(), "Generated import accidentally created a user-upload tray item.");
    }
    {
        AttachmentLibrary reopened(files, database, audit);
        Check(reopened.ReadImageSource(importedReference).has_value(), "An imported generated source was lost after library reopen.");
        std::string error;
        Check(reopened.Forget(importedReference.id, error) && !std::filesystem::exists(importedPath) &&
                  !reopened.ReadImageSource(importedReference),
            "Forget did not remove the sole canonical generated copy: " + error);
    }
    AttachmentLibrary retired(files, database, audit);
    auto expired = VerifiedGeneratedFixture(outputRoot, "job-expired");
    const auto expiredResult = retired.ImportGeneratedImage(expired, outputRoot, [] { return false; });
    Check(!expiredResult.succeeded && !std::filesystem::exists(expired.path) && retired.List().empty(),
        "Expired admission left a verified unpublished generated output or a library association.");
    auto stopped = VerifiedGeneratedFixture(outputRoot, "job-stopped");
    std::stop_source stop;
    stop.request_stop();
    Check(!retired.ImportGeneratedImage(
                      stopped, outputRoot, [] { return true; }, stop.get_token())
                  .succeeded &&
              !std::filesystem::exists(stopped.path) && retired.List().empty(),
        "Cancelled import retained a verified unpublished output.");
    for (const int expiresAt : {2, 3, 4})
    {
        auto late = VerifiedGeneratedFixture(outputRoot, "job-late-" + std::to_string(expiresAt));
        int calls = 0;
        const auto lateResult = retired.ImportGeneratedImage(late, outputRoot, [&] { return ++calls < expiresAt; });
        Check(!lateResult.succeeded && !std::filesystem::exists(late.path) && retired.List().empty(),
            "Late admission retirement left a generated association or duplicate output.");
    }
    auto foreign = VerifiedGeneratedFixture(directory.root / "foreign", "job-foreign");
    Check(!retired.ImportGeneratedImage(foreign, outputRoot, [] { return true; }).succeeded && std::filesystem::exists(foreign.path),
        "Import accepted or deleted a file outside its bound output root.");
    auto changed = VerifiedGeneratedFixture(outputRoot, "job-changed");
    {
        std::ofstream output(changed.path, std::ios::binary | std::ios::app);
        output << "changed";
    }
    Check(!retired.ImportGeneratedImage(changed, outputRoot, [] { return false; }).succeeded && std::filesystem::exists(changed.path),
        "Import accepted or deleted bytes that no longer matched the generated receipt.");
    auto missingSource = VerifiedGeneratedFixture(outputRoot, "job-missing-source");
    auto& expected = missingSource.expectedArtifact;
    auto& receipt = missingSource.receipt;
    expected.mode = receipt.mode = revia::visual::ImageRequestMode::ReferenceToImage;
    expected.pipeline = receipt.pipeline = "StableDiffusionImg2ImgPipeline";
    expected.requestedStrength = receipt.requestedStrength = expected.effectiveStrength = receipt.effectiveStrength = 0.5;
    expected.denoisingSteps = receipt.denoisingSteps = 2;
    expected.sourceId = receipt.sourceId = "forgotten-parent";
    expected.sourceDigest = receipt.sourceDigest = receipt.preparedSourceDigest = revia::audit::ContentDigest(TinyPng());
    expected.sourcePreparation = receipt.sourcePreparation = "contain-pad-rgb-v1";
    Check(revia::visual::VerifyImageArtifact(expected, receipt, verificationError),
        "The reference fixture failed structural verification: " + verificationError);
    const auto orphan = retired.ImportGeneratedImage(missingSource, outputRoot, [] { return true; });
    Check(!orphan.succeeded && !std::filesystem::exists(missingSource.path) && retired.List().empty(),
        "A permissive caller published an edit whose source was no longer in the current library.");
    auto parentArtifact = VerifiedGeneratedFixture(outputRoot, "job-independent-parent");
    const auto parent = retired.ImportGeneratedImage(parentArtifact, outputRoot, [] { return true; });
    Check(parent.succeeded, "The independent parent fixture did not import: " + parent.error);
    auto child = VerifiedGeneratedFixture(outputRoot, "job-independent-child");
    child.expectedArtifact = expected;
    child.expectedArtifact.path = child.path;
    child.expectedArtifact.jobId = "job-independent-child";
    child.expectedArtifact.sourceId = parent.reference.id;
    child.expectedArtifact.sourceDigest = parent.reference.digest;
    child.receipt = receipt;
    child.receipt.path = child.path;
    child.receipt.jobId = child.expectedArtifact.jobId;
    child.receipt.sourceId = parent.reference.id;
    child.receipt.sourceDigest = parent.reference.digest;
    const auto independentlyRetained = retired.ImportGeneratedImage(child, outputRoot, [] { return true; });
    Check(independentlyRetained.succeeded,
        "A current generated parent did not yield an independently retained edit: " + independentlyRetained.error);
    std::string forgetError;
    Check(retired.Forget(parent.reference.id, forgetError) && retired.ReadImageSource(independentlyRetained.reference).has_value(),
        "Forgetting a parent incorrectly retired its already committed independent generated child: " + forgetError);
}

void TestGeneratedArtifactContractsRemainExplicit()
{
    conversationMediaReference reference;
    Check(reference.origin == MediaOrigin::UserAttachment, "Legacy references no longer default to user attachments.");
    reference.origin = MediaOrigin::GeneratedArtifact;
    revia::attachments::StoredImageSource source{reference, {}};
    revia::attachments::GeneratedImageImportResult imported;
    revia::attachments::StoredDocument stored;
    Check(stored.origin == MediaOrigin::UserAttachment && !stored.generated,
        "Legacy stored documents no longer default to upload provenance.");
    Check(source.reference.origin == MediaOrigin::GeneratedArtifact && !imported.succeeded && imported.storedPath.empty(),
        "Generated source/import contracts lost explicit origin or a failed default.");
}
}

void RunGeneratedArtifactContinuityTests()
{
    TestGeneratedArtifactContractsRemainExplicit();
    TestStoredGeneratedOriginPersistsAndLegacyRowsMigrate();
    TestGeneratedAssistantMetadataKeepsScopedVersionSelection();
    TestDedicatedGeneratedAssistantArchiveKeepsGenericGuardAndScope();
    TestImageSourceReadIgnoresVisionButAuthenticatesCurrentOrigin();
    TestGeneratedImportTransfersExactlyOneOwnedOutput();
}

#ifdef REVIA_GENERATED_ARTIFACT_CONTINUITY_STANDALONE
int main(int argc, char** argv)
{
    try
    {
        const std::string suite = argc > 1 ? argv[1] : "all";
        if (suite == "store")
            TestStoredGeneratedOriginPersistsAndLegacyRowsMigrate();
        else if (suite == "selection")
            TestGeneratedAssistantMetadataKeepsScopedVersionSelection();
        else if (suite == "archive")
            TestDedicatedGeneratedAssistantArchiveKeepsGenericGuardAndScope();
        else if (suite == "source")
            TestImageSourceReadIgnoresVisionButAuthenticatesCurrentOrigin();
        else if (suite == "import")
            TestGeneratedImportTransfersExactlyOneOwnedOutput();
        else
            RunGeneratedArtifactContinuityTests();
        std::cout << "Generated artifact continuity checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Generated continuity failure: " << error.what() << '\n';
        return 1;
    }
}
#endif
