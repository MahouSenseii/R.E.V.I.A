#include "reviaSessionTestAccess.h"

#include "Audit/contentDigest.h"
#include "Visual/imageArtifact.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>

namespace
{
using Access = revia::runtime::ReviaSessionTestAccess;
using revia::runtime::RuntimeEvent;
using revia::runtime::RuntimeEventKind;
using revia::tests::Check;

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

struct Fixture
{
    revia::tests::ScopedTestDirectory directory;
    revia::runtime::ReviaSession session{
        revia::runtime::CompanionPaths(directory.root, {"generated-runtime", "Generated runtime fixture", "assistant", false})};
    std::vector<RuntimeEvent> events;
    revia::runtime::RuntimeEventBus::SubscriptionId subscription = 0;

    Fixture()
    {
        Access::ConfigureGeneratedMediaRuntime(session);
        std::string error;
        Check(session.SetAudience({revia::identity::AudienceKind::Private, "generated-runtime-room", 0, {}}, error), error);
        Check(!Access::IntroduceTaskSpeaker(session, "GeneratedFixtureOwner").empty(), "The fixture participant was not attributed.");
        (void)Access::StudioInput(session, revia::agents::InputSource::Typed);
        Access::OpenConversationArchive(session, directory.root / "archive.db", "generated-runtime-chat");
        subscription = session.Events().Subscribe([this](const RuntimeEvent& event) { events.push_back(event); });
    }

    ~Fixture()
    {
        session.Events().Unsubscribe(subscription);
        Access::MarkStudioStarted(session, false);
    }

    revia::attachments::GeneratedImageImportResult Import(const std::string& job)
    {
        const auto bytes = Read(std::filesystem::path(__FILE__).parent_path() / "Fixtures/Attachments/tiny.png");
        Check(!bytes.empty(), "The actual PNG fixture was unavailable.");
        const auto output = directory.root / "owned-output";
        std::filesystem::create_directories(output);
        revia::visual::ImageResult artifact;
        artifact.succeeded = true;
        artifact.path = output / (job + ".png");
        {
            std::ofstream file(artifact.path, std::ios::binary);
            file << bytes;
        }
        auto& expected = artifact.expectedArtifact;
        expected.path = artifact.path;
        expected.jobId = job;
        expected.model = "sd-turbo";
        expected.width = 4;
        expected.height = 3;
        expected.pipeline = "StableDiffusionPipeline";
        expected.variant = "fp16";
        expected.seed = 42;
        expected.requestedSteps = expected.totalInferenceSteps = expected.denoisingSteps = 4;
        auto& receipt = artifact.receipt;
        receipt.path = expected.path;
        receipt.jobId = job;
        receipt.model = expected.model;
        receipt.width = expected.width;
        receipt.height = expected.height;
        receipt.provider = expected.provider;
        receipt.pipeline = expected.pipeline;
        receipt.variant = expected.variant;
        receipt.seed = expected.seed;
        receipt.requestedSteps = receipt.totalInferenceSteps = receipt.denoisingSteps = 4;
        receipt.sha256 = revia::audit::ContentDigest(bytes);
        std::string error;
        Check(revia::visual::VerifyImageArtifact(expected, receipt, error), "The CPU PNG receipt failed native verification: " + error);
        const auto imported = Access::Attachments(session).ImportGeneratedImage(artifact, output, [] { return true; });
        Check(imported.succeeded, "The CPU PNG fixture did not import: " + imported.error);
        return imported;
    }

    void Remember(const conversationMediaReference& reference, const std::string& text)
    {
        Access::SeedGeneratedContext(session, reference, text);
        std::string error;
        Check(Access::RecordGeneratedArchive(session, reference, text, error), error);
    }
};

void TestDiscardRetiresLogicalIdentityWhenPixelsAreUnavailable()
{
    Fixture fixture;
    auto& library = Access::Attachments(fixture.session);
    for (const std::string mode : {"missing", "tampered", "nonempty-directory"})
    {
        const auto imported = fixture.Import("discard-" + mode);
        const auto sentinel = "generated_retirement_" + mode;
        fixture.Remember(imported.reference, sentinel);
        if (mode == "tampered")
        {
            std::ofstream file(imported.storedPath, std::ios::binary | std::ios::trunc);
            file << "not the original PNG";
        }
        else
        {
            Check(std::filesystem::remove(imported.storedPath), "Could not remove the test-owned canonical PNG.");
            if (mode == "nonempty-directory")
            {
                std::filesystem::create_directory(imported.storedPath);
                std::ofstream file(imported.storedPath / "unrelated.txt");
                file << "preserve this unrelated content";
            }
        }
        Check(!library.ImageReferencesCurrent({imported.reference}), "The fixture did not retire actual pixel availability.");
        fixture.events.clear();
        Access::DiscardGeneratedMedia(fixture.session, imported.reference);
        Check(library.List().empty(), "Discard retained a generated logical record after its pixels became unavailable: " + mode);
        const auto queued = Access::PendingGeneratedInvalidations(fixture.session);
        Check(std::find(queued.begin(), queued.end(), imported.reference.id) != queued.end(),
            "Discard did not queue working-context invalidation: " + mode);
        Access::DrainGeneratedInvalidations(fixture.session);
        const auto context = Access::WorkingContext(fixture.session);
        Check(std::none_of(context.begin(), context.end(), [&](const auto& message) { return message.content == sentinel; }) &&
                  std::any_of(context.begin(), context.end(),
                      [&](const auto& message) { return message.mediaReferences == std::vector{imported.reference}; }),
            "Retirement failed to clear generated analysis or discarded the unavailable identity tombstone.");
        const auto archived = Access::RawArchivedSession(fixture.session, "generated-runtime-chat");
        Check(std::none_of(archived.begin(), archived.end(), [&](const auto& turn) { return turn.content == sentinel; }),
            "Discard retained archived generated analysis.");
        if (mode == "nonempty-directory")
        {
            Check(Read(imported.storedPath / "unrelated.txt") == "preserve this unrelated content",
                "A failed physical cleanup deleted unrelated directory contents.");
            Check(
                std::any_of(fixture.events.begin(), fixture.events.end(), [](const auto& event)
                    { return event.kind == RuntimeEventKind::Warning && event.message.find("could not be retired") != std::string::npos; }),
                "A failed physical cleanup did not report its operational failure.");
        }
    }
    const auto current = fixture.Import("wrong-digest");
    auto wrong = current.reference;
    wrong.digest = std::string(64, '0');
    Access::DiscardGeneratedMedia(fixture.session, wrong);
    Check(library.ImageReferencesCurrent({current.reference}) && std::filesystem::exists(current.storedPath) &&
              Access::PendingGeneratedInvalidations(fixture.session).empty(),
        "A mismatched digest retired a current generated artifact.");
}

void TestQuietRetainsTheGeneratedCardAndEpochRetiresPendingVideoReports()
{
    Fixture fixture;
    auto& library = Access::Attachments(fixture.session);
    const auto imported = fixture.Import("quiet-completion");
    const auto finish = Access::ArmGeneratedTaskCompletion(fixture.session, imported.reference);
    Check(Access::GeneratedSpeechAdmitted(fixture.session), "The initial speech admission control was already closed.");
    Check(Access::SetGeneratedQuiet(fixture.session, true) && !Access::GeneratedSpeechAdmitted(fixture.session),
        "RequestQuiet did not close speech admission.");
    fixture.events.clear();
    finish();
    Check(library.ImageReferencesCurrent({imported.reference}) && std::filesystem::exists(imported.storedPath),
        "Quiet mode discarded an already completed generated artifact.");
    Check(std::count_if(fixture.events.begin(), fixture.events.end(),
              [&](const auto& event)
              {
                  return event.kind == RuntimeEventKind::AssistantMessage && event.generatedMediaReference == imported.reference &&
                         event.conversationMediaEpoch == fixture.session.ConversationMediaEpoch();
              }) == 1 &&
              std::count_if(fixture.events.begin(), fixture.events.end(),
                  [](const auto& event) { return event.kind == RuntimeEventKind::AssistantMessage; }) == 1,
        "Quiet completion did not deliver exactly one current host-typed generated card.");
    Check(!Access::Speech(fixture.session).HasPendingSpeech() &&
              std::none_of(fixture.events.begin(), fixture.events.end(),
                  [](const auto& event) { return event.component == "Voice" || event.kind == RuntimeEventKind::ReplyFragment; }),
        "Quiet generated completion submitted speech.");
    Check(Access::SetGeneratedQuiet(fixture.session, false) && Access::GeneratedSpeechAdmitted(fixture.session),
        "The epoch control retained quiet mode and would mask stale report admission.");
    const auto pending = Access::ArmGeneratedTaskCompletion(fixture.session);
    Access::RetireGeneratedConversation(fixture.session);
    fixture.events.clear();
    pending();
    Check(std::none_of(fixture.events.begin(), fixture.events.end(),
              [](const auto& event) { return event.kind == RuntimeEventKind::AssistantMessage; }),
        "A pending video task with no result reported into a later conversation epoch.");
    Check(library.ImageReferencesCurrent({imported.reference}), "Retiring a pending report removed an independently retained artifact.");
}
}

void TestFreshAnimationSourceReachesVideoAvailability()
{
    Fixture fixture;
    const auto source = std::filesystem::path(__FILE__).parent_path() / "Fixtures/Attachments/tiny.png";
    const auto staged = fixture.session.AttachFile(source);
    Check(staged.status == revia::attachments::IntakeStatus::Accepted, "The animation-source PNG was not accepted.");
    const auto answer = fixture.session.SubmitWithAttachments("Animate this image into a short video.", {staged.attachment.id});
    Check(!answer.succeeded && answer.generatedMediaReferences.empty() && answer.reason == "Video generation is disabled.",
        "A fresh animation source was replaced by historical image availability: " + answer.reason);
}

void RunGeneratedMediaRuntimeTests()
{
    TestDiscardRetiresLogicalIdentityWhenPixelsAreUnavailable();
    TestQuietRetainsTheGeneratedCardAndEpochRetiresPendingVideoReports();
    TestFreshAnimationSourceReachesVideoAvailability();
    std::cout << "Generated runtime retirement and quiet typed-card delivery checks passed.\n";
}

#ifdef REVIA_GENERATED_MEDIA_RUNTIME_STANDALONE
int main()
{
    try
    {
        RunGeneratedMediaRuntimeTests();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Generated media runtime failure: " << error.what() << '\n';
        return 1;
    }
}
#endif
