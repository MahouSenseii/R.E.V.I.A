#include "Attachments/videoSelection.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
void Check(bool value, const char* reason)
{
    if (!value)
        throw std::runtime_error(reason);
}

void TestIntervals()
{
    using namespace revia::attachments;
    auto interval = ParseVideoInterval("What happens around 00:12?", 20000);
    Check(interval.valid && interval.explicitInterval && interval.startMs == 11500 && interval.endMs == 12500,
        "A timestamp follow-up did not select its bounded interval.");
    interval = ParseVideoInterval("Compare 00:02.250 to 00:03.750.", 20000);
    Check(interval.valid && interval.startMs == 2250 && interval.endMs == 3750, "Fractional interval endpoints were lost.");
    Check(!ParseVideoInterval("Look at 00:99.", 20000).valid, "An invalid timestamp was silently guessed.");
    Check(!ParseVideoInterval("What happens at 00:45?", 20000).valid, "An out-of-clip timestamp was silently clamped.");
    Check(!ParseVideoInterval("Compare 00:15 to 00:10.", 20000).valid, "A reversed interval was silently reordered.");
    Check(!ParseVideoInterval("Compare 00:01, 00:02 and 00:03.", 20000).valid, "Too many timestamp endpoints were silently dropped.");
    interval = ParseVideoInterval("Inspect it more densely.", 20000, 11500, 12500);
    Check(interval.valid && interval.startMs == 11500 && interval.endMs == 12500, "A denser follow-up discarded the previous interval.");
    interval = ParseVideoInterval("Describe the clip.", 20000);
    Check(interval.valid && !interval.explicitInterval && interval.startMs == 0 && interval.endMs == 19999,
        "Default clip sampling lost its finite source coverage.");
}

void TestGeneratedVideosKeepOriginChronologyAndScope()
{
    using namespace revia::attachments;
    revia::memory::MemoryScope scope;
    scope.companionId = "generated-video-companion";
    scope.participantId = "generated-video-person";
    scope.participantSource = revia::identity::SpeakerSource::ExplicitIntroduction;
    scope.audience = {revia::identity::AudienceKind::Private, "generated-video-room", 4, {}};
    conversationMediaReference uploaded{"uploaded", std::string(64, 'a'), "uploaded.mp4", "video/mp4", 384, 256};
    uploaded.durationMs = 1960;
    uploaded.sampleEndMs = 1959;
    auto first = uploaded;
    first.id = "generated-one";
    first.name = "generated-one.mp4";
    first.origin = MediaOrigin::GeneratedArtifact;
    auto second = first;
    second.id = "generated-two";
    second.name = "generated-two.mp4";
    conversationMessage user{"user", "An uploaded clip.", scope.participantId, scope};
    user.mediaReferences = {uploaded};
    conversationMessage generated{"assistant", "A generated clip is ready.", scope.participantId, scope};
    generated.mediaReferences = {first};
    auto newer = generated;
    newer.mediaReferences = {second};
    const std::vector history{user, generated, newer};
    Check(SelectConversationVideos("Describe the generated clip.", history, scope).references == std::vector{second},
        "A private generated assistant clip did not become the current video source.");
    Check(SelectConversationVideos("Describe the uploaded clip.", history, scope).references == std::vector{uploaded},
        "Generated video was misrepresented as an uploaded clip.");
    Check(SelectConversationVideos("Inspect 00:00.100 to 00:00.500 in that clip.", history, scope).references == std::vector{second},
        "A timed generated-video follow-up lost current identity.");
    Check(SelectConversationVideos("Show the previous version.", history, scope).references == std::vector{first},
        "Generated video versions lost chronological previous selection.");
    Check(SelectConversationVideos("Describe the first generated clip.", history, scope).references == std::vector{first},
        "Generated-only clip ordinals included a user upload.");
    Check(SelectConversationVideos("Describe the first clip.", history, scope).references == std::vector{uploaded},
        "General clip chronology no longer includes the original upload.");
    Check(SelectConversationVideos("Compare the uploaded and generated clips.", history, scope).references.empty(),
        "A multi-video comparison silently discarded one origin.");
    auto forged = generated;
    forged.mediaReferences = {uploaded};
    Check(SelectConversationVideos("Describe the clip.", {forged}, scope).references.empty(),
        "Generic assistant upload metadata was promoted into video authority.");
    auto foreign = scope;
    foreign.participantId = "other-person";
    Check(SelectConversationVideos("Describe the generated clip.", history, foreign).references.empty(),
        "A generated video escaped its participant scope.");
    auto picture = user;
    picture.mediaReferences = {{"picture", std::string(64, 'b'), "picture.png", "image/png", 4, 3}};
    Check(SelectConversationMedia("Describe it.", {picture, newer}, scope).references == std::vector{second},
        "A new generated video left implicit focus on an older still image.");
    picture.role = "assistant";
    picture.mediaReferences.front().origin = MediaOrigin::GeneratedArtifact;
    Check(SelectConversationMedia("Describe it.", {user, picture}, scope).references == picture.mediaReferences,
        "A new generated image left implicit focus on an older video.");
}

void TestPureTranscriptRequests()
{
    using revia::attachments::RequestsOnlyVideoTranscript;
    const std::vector<std::pair<std::string, bool>> cases = {
        {"Transcribe the speech in the attached clip with timestamps. Keep this audio transcript separate from visual evidence.", true},
        {"Transcribe the speech at 00:01.000 to 00:06.000 in that clip with timestamps. Keep the uncertain audio transcript separate from "
         "visual evidence.",
            true},
        {"Transcribe the speech at 00:01.000 to 00:06.000 in the attached clip. State that this is uncertain ASR audio evidence; do not "
         "claim visual access.",
            true},
        {"Transcribe any clear speech in the attached clip with timestamps. If no speech is established, say so. Do not invent words or "
         "identify non-speech sounds from frames.",
            true},
        {"Transcribe the clip at 00:01 to 00:06.", true}, {"Please transcribe the audio from that video with timestamps.", true},
        {"Can you transcribe the speech?", true}, {"Transcribe speech.", true}, {"Transcribe this video.", true},
        {"Transcribe only the speech in this clip.", true}, {"Transcribe the clip at 00:99.", true},
        {"Transcribe the speech and describe the center shape.", false}, {"Transcribe the clip. Read its label.", false},
        {"Read the on-screen transcript.", false}, {"Transcribe the on-screen text.", false},
        {"Read the words \"Transcribe the speech\" in this image.", false}, {"\"Transcribe the speech in the attached clip.\"", false},
        {"'Transcribe the speech.'", false}, {"`Transcribe the clip`", false}, {"She said: transcribe the clip.", false},
        {"Do not transcribe the clip.", false}, {"Transcribe the audio and translate it into French.", false},
        {"Transcribe the audio; write a poem about it.", false}, {"Transcribe the speech. Explain why the person sounds angry.", false},
        {"How reliable is the transcript?", false}, {"What is the final word in that transcript?", false},
        {"Tell me a story about a transcript.", false}, {"How does speech transcription work?", false},
        {"Transcribe the clip with timestamps. Keep the uncertain audio transcript separate from visual evidence and describe the first "
         "frame.",
            false},
        {"Transcribe 001.mp4.", false}, {"Transcribe the speech at 1 to 6 seconds.", false}, {"Transcribe the speech at time.", false},
        {"Transcribe the speech. ;", false}};
    for (const auto& [input, expected] : cases)
        Check(RequestsOnlyVideoTranscript(input) == expected,
            "Pure ASR delivery accepted mixed/quoted content or declined its closed grammar.");
    Check(!RequestsOnlyVideoTranscript(std::string(4097, 'x')), "An oversized pure transcript request was accepted.");
}

void TestIdentityAndScope()
{
    using namespace revia::attachments;
    revia::memory::MemoryScope scope;
    scope.companionId = "companion";
    scope.participantId = "local:user";
    scope.audience = {revia::identity::AudienceKind::Private, "private", 4, {}};
    conversationMediaReference first{"one", std::string(64, 'a'), "001.mp4", "video/mp4", 640, 360};
    first.durationMs = 20000;
    first.sampleEndMs = 19999;
    auto second = first;
    second.id = "two";
    second.name = "002.webm";
    second.mimeType = "video/webm";
    conversationMessage earlier{"user", "Inspect it.", scope.participantId, scope};
    earlier.mediaReferences = {first};
    auto later = earlier;
    later.mediaReferences = {second};
    Check(SelectConversationVideos("What happens at 00:12?", {earlier}, scope).references == std::vector{first},
        "A timestamp follow-up lost its source clip.");
    Check(SelectConversationVideos("Describe the first video.", {earlier, later}, scope).references == std::vector{first},
        "A clip ordinal selected the newest clip.");
    Check(SelectConversationVideos("Describe 001.mp4.", {earlier, later}, scope).references == std::vector{first},
        "A named clip selected the wrong source.");
    Check(SelectConversationVideos("Describe absent.mp4.", {earlier}, scope).references.empty(),
        "A missing named clip silently selected another clip.");
    Check(SelectConversationVideos("Compare the videos.", {earlier, later}, scope).references.empty(),
        "A multiple-clip request silently discarded one source.");
    Check(!SelectConversationVideos("How are you today?", {earlier}, scope).requested, "Ordinary conversation inherited a clip.");
    Check(!SelectConversationVideos("How does video compression work?", {earlier}, scope).requested,
        "A general technical question inherited a clip.");
    Check(
        SelectConversationImages("Describe the image.", {earlier}, scope).references.empty(), "A video was treated as an uploaded image.");
    auto foreign = scope;
    foreign.audience.kind = revia::identity::AudienceKind::Shared;
    Check(SelectConversationVideos("Describe the clip.", {earlier}, foreign).references.empty(), "Shared scope inherited a private clip.");
    Check(SelectConversationVideos("What happens at 00:12?", {}, scope).references.empty(), "A new chat inherited a clip.");
    Check(!SelectConversationMedia("What does 12:30 mean on a clock?", {}, scope).requested,
        "An ordinary timestamp question became a request for a missing video.");
    Check(!SelectConversationMedia("Our meeting starts at 12:30.", {earlier}, scope).requested,
        "An ordinary dated statement inherited a clip.");
    Check(SelectConversationVideos("Transcribe it.", {earlier}, scope).references == std::vector{first},
        "A direct transcript follow-up lost the current clip.");
    Check(SelectConversationVideos("Describe the second transition in that clip.", {earlier}, scope).references == std::vector{first},
        "An ordinal inside a clip was mistaken for another uploaded clip.");
    conversationMediaReference picture{"picture", std::string(64, 'b'), "003.png", "image/png", 640, 360};
    auto image = earlier;
    image.mediaReferences = {picture};
    Check(SelectConversationMedia("Read 12:30 from that image.", {image, earlier}, scope).references == std::vector{picture},
        "An explicit image request was hijacked by a video timestamp.");
}

void TestExplicitClipIdentifiersNeverSelectAnotherSource()
{
    using namespace revia::attachments;
    revia::memory::MemoryScope scope;
    scope.companionId = "companion";
    scope.participantId = "local:user";
    scope.audience = {revia::identity::AudienceKind::Private, "private", 4, {}};
    conversationMediaReference first{"one", std::string(64, 'a'), "001.mp4", "video/mp4", 640, 360};
    first.durationMs = 20000;
    first.sampleEndMs = 19999;
    auto second = first;
    second.id = "two";
    second.name = "002.webm";
    second.mimeType = "video/webm";
    conversationMessage earlier{"user", "Inspect it.", scope.participantId, scope};
    earlier.mediaReferences = {first};
    auto later = earlier;
    later.mediaReferences = {second};
    const std::vector<conversationMessage> history{earlier, later};
    Check(SelectConversationVideos("Describe the previous clip.", history, scope).references == std::vector{first},
        "The previous clip silently selected the latest unique upload.");
    auto previous = SelectConversationVideos("Describe the previous clip.", {earlier}, scope);
    Check(previous.requested && previous.references.empty() && !previous.notice.empty(),
        "A nonexistent previous clip silently selected the only upload.");
    auto reinspected = earlier;
    reinspected.mediaReferences.front().sampleStartMs = 11500;
    reinspected.mediaReferences.front().sampleEndMs = 12500;
    Check(SelectConversationVideos("Describe the previous clip.", {earlier, later, reinspected}, scope).references ==
              reinspected.mediaReferences,
        "Reinspection changed unique-upload ordering for the previous clip.");
    Check(SelectConversationVideos("Describe the last clip.", {earlier, later, reinspected}, scope).references == std::vector{second},
        "The last unique upload was replaced by a prior clip's reinspection.");
    for (const char* input : {"Describe prefix_001.mp4.", "Describe x001.mp4.", "Describe 001.mp4.backup.", "Describe folder/001.mp4.",
             "Compare 001.mp4 with absent.webm.", "Compare absent.webm with 001.mp4.", "Compare 001.mp4 with the second clip.",
             "Compare the first clip and the second clip.", "Compare first and second clip.", "Compare clip 1 and clip 2.",
             "Compare clip 1 and 2.", "Describe clip1-3.", "Describe clip1-final.", "Describe clip1/2.", "Describe clip 0.",
             "Describe clip 257.", "Describe clip 999999999999999999999999.", "Describe video#5."})
    {
        const auto selected = SelectConversationVideos(input, history, scope);
        Check(selected.requested && selected.references.empty() && !selected.notice.empty(),
            "A conflicting, unavailable or partial clip identifier selected an unrelated source.");
    }
    for (const char* input : {"Describe \"001.mp4\".", "Describe 001.mp4...", "Describe 001.mp4, the first clip."})
        Check(SelectConversationVideos(input, history, scope).references == std::vector{first},
            "An exact literal clip name or matching identifiers lost their source.");
    for (const char* input :
        {"Describe clip2.", "Describe clip#2.", "Describe clip #2.", "Describe video2.", "Describe video#2.", "Describe video #2."})
        Check(SelectConversationVideos(input, history, scope).references == std::vector{second},
            "A supported numeric clip identifier lost its exact upload.");
    for (const char* input : {"Describe the videogame.", "Explain the videographer's job.", "Explain the video games.",
             "Explain the video compression.", "How does video compression work?"})
        Check(!SelectConversationVideos(input, history, scope).requested, "A generic video-related word inherited private clip pixels.");
    auto namedOrdinal = earlier;
    namedOrdinal.mediaReferences.front().name = "video2.mp4";
    Check(SelectConversationVideos("Describe video2.mp4.", {namedOrdinal, later}, scope).references == namedOrdinal.mediaReferences,
        "A numeric token inside the exact filename nominated a different clip.");
    auto picture = earlier;
    picture.mediaReferences = {{"picture", std::string(64, 'c'), "003.png", "image/png", 640, 360}};
    const auto mixed = SelectConversationMedia("Compare 001.mp4 with 003.png.", {earlier, picture}, scope);
    Check(mixed.requested && mixed.references.empty() && !mixed.notice.empty(),
        "A mixed explicit image/video request silently dropped its clip.");
    picture.mediaReferences.front().name = "clip#2.png";
    Check(SelectConversationMedia("Describe clip#2.png.", {earlier, later, picture}, scope).references == picture.mediaReferences,
        "A clip-like token inside an exact image filename hijacked image selection.");
    std::vector<conversationMessage> many;
    for (int index = 0; index < 256; ++index)
    {
        auto message = earlier;
        message.mediaReferences.front().id = "clip-" + std::to_string(index + 1);
        message.mediaReferences.front().name = std::to_string(index + 1) + ".mp4";
        many.push_back(std::move(message));
    }
    Check(SelectConversationVideos("Describe clip#256.", many, scope).references == many.back().mediaReferences,
        "The maximum supported numeric clip identifier was not selectable.");
}
}

void RunVideoSelectionTests()
{
    TestGeneratedVideosKeepOriginChronologyAndScope();
    TestIntervals();
    TestPureTranscriptRequests();
    TestIdentityAndScope();
    TestExplicitClipIdentifiersNeverSelectAnotherSource();
}

#ifdef REVIA_VIDEO_SELECTION_STANDALONE
int main()
{
    try
    {
        RunVideoSelectionTests();
        std::cout << "Video interval and scoped selection checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
