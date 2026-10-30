#include "testSupport.h"

#include "Attachments/attachmentLibrary.h"
#include "Attachments/attachmentPrompt.h"
#include "Attachments/videoSelection.h"
#include "Core/utf8.h"
#include "Memory/conversationArchive.h"

#include <iostream>

namespace
{
void TestVideoSignaturesAndReferences()
{
    using namespace revia::attachments;
    using revia::tests::Check;
    std::string reason;
    const std::string mp4("\0\0\0\x18"
                          "ftypisom\0\0\0\0isommp42",
        24);
    const auto mp4Kind = DetectKind(mp4, ".mp4", reason);
    Check(mp4Kind && ToString(*mp4Kind) == "video", "MP4 was not recognized as explicit video intake.");
    Check(!IsDocument(AttachmentKind::Video), "Video was classified as searchable document text.");
    revia::tests::ScopedTestDirectory directory;
    AttachmentLibrary library(directory.root / "Files", directory.root / "documents.db", directory.root / "audit.jsonl");
    const auto corrupt = library.StageBytes("neutral.mp4", mp4);
    Check(corrupt.status == IntakeStatus::Refused && library.Staged().empty(), "An unreadable video was accepted or retained.");
    std::stop_source cancelled;
    cancelled.request_stop();
    Check(library.StageBytes("cancelled.mp4", mp4, cancelled.get_token()).status == IntakeStatus::Refused,
        "Already canceled video intake accepted a file.");
    Check(!std::filesystem::exists(directory.root / "Files") || std::filesystem::is_empty(directory.root / "Files"),
        "Failed video intake left a private source copy.");
}

void TestVideoArchiveMetadata()
{
    using revia::tests::Check;
    revia::tests::ScopedTestDirectory directory;
    revia::memory::ConversationArchive archive((directory.root / "archive.db").string());
    revia::memory::MemoryScope scope;
    scope.companionId = "companion";
    scope.participantId = "fixture-user";
    scope.participantSource = revia::identity::SpeakerSource::ExplicitIntroduction;
    scope.audience = {revia::identity::AudienceKind::Private, "private", 4, {}};
    conversationMediaReference reference{"clip", std::string(64, 'a'), "001.mp4", "video/mp4", 640, 360};
    reference.durationMs = 20000;
    reference.sampleStartMs = 11500;
    reference.sampleEndMs = 12500;
    std::string error;
    Check(archive.BeginSession("video", error) &&
              archive.Record("video", "user", "Inspect the clip at 00:12.", error, scope, {}, {reference}),
        "Video metadata could not be archived: " + error);
    const auto restored = archive.LoadSessionTail("video", 5, scope);
    Check(restored.size() == 1 && restored.front().mediaReferences == std::vector{reference},
        "Restoration lost source video identity or selected interval.");
    auto anonymous = scope;
    anonymous.participantId = "local:user";
    Check(archive.LoadSessionTail("video", 5, anonymous).empty(), "Anonymous attribution restored a private video.");
    auto invalid = reference;
    invalid.sampleEndMs = invalid.durationMs;
    Check(!archive.Record("video", "user", "Invalid interval.", error, scope, {}, {invalid}), "Out-of-clip sample metadata was archived.");
    auto shared = scope;
    shared.audience.kind = revia::identity::AudienceKind::Shared;
    Check(!archive.Record("video", "user", "A shared clip.", error, shared, {}, {reference}),
        "A private clip reference entered a shared archive turn.");
}

void TestLongTranscriptPreservesSampleEvidence()
{
    using namespace revia::attachments;
    using revia::tests::Check;
    AttachedFileView video;
    video.kind = AttachmentKind::Video;
    video.name = "001.mp4";
    video.imageVisible = true;
    video.note = "Sampled-frame analysis only. Image 1 = 00:12.200. Motion between frames is uncertain. ASR evidence begins: " +
                 std::string(20000, 'x') + " omitted-tail";
    const auto reference = BuildAttachmentBlock({video});
    Check(reference.size() <= 16000 && reference.find("Image 1 = 00:12.200") != std::string::npos &&
              reference.find("Motion between frames is uncertain") != std::string::npos &&
              reference.find("ASR evidence begins") != std::string::npos && reference.find("narrower interval") != std::string::npos &&
              reference.find("omitted-tail") == std::string::npos && revia::utf8::IsValid(reference),
        "A long transcript dropped sample mapping/uncertainty or concealed omitted evidence instead of retaining a bounded excerpt.");
}
}

void RunVideoAttachmentTests()
{
    TestVideoSignaturesAndReferences();
    TestVideoArchiveMetadata();
    TestLongTranscriptPreservesSampleEvidence();
}
