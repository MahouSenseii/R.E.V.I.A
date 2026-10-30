#include "Attachments/mediaSelection.h"
#include "Core/conversationContext.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void Check(const bool value, const std::string& message)
{
    if (!value)
        throw std::runtime_error(message);
}

void TestGeneratedVersionAliasesRetainChronologyAndUnavailableIdentity()
{
    revia::memory::MemoryScope scope;
    scope.companionId = "generated-companion";
    scope.participantId = "generated-owner";
    scope.participantSource = revia::identity::SpeakerSource::ExplicitIntroduction;
    scope.audience = {revia::identity::AudienceKind::Private, "generated-room", 4, {}};
    conversationMediaReference original{"original", std::string(64, 'a'), "original.png", "image/png", 30, 20};
    original.origin = MediaOrigin::GeneratedArtifact;
    auto variation = original;
    variation.id = "variation";
    variation.digest = std::string(64, 'b');
    variation.name = "variation.png";
    conversationMessage first{"assistant", "The original is ready.", scope.participantId, scope};
    first.mediaReferences = {original};
    auto second = first;
    second.content = "The variation is ready.";
    second.mediaReferences = {variation};
    conversationContext context;
    context.AddMessage(first);
    context.AddMessage(second);
    context.InvalidateMediaAnalysis(variation.id);
    const auto archived = context.GetRecentMessages();
    context.Clear();
    context.RestoreMessages(archived, 12);
    const auto history = context.GetMediaHistory();
    Check(history.size() == 2, "The restored generated-version fixture lost its source records.");
    using revia::attachments::SelectConversationImages;
    const auto retained = SelectConversationImages(
        "Look again at the first generated version in this restored conversation. Describe what is visible.", history, scope);
    Check(retained.requested && retained.references == std::vector{original},
        "The actual restored-original request lost its chronological generated source.");
    const auto unavailable = SelectConversationImages("Look at the second generated version and describe it.", history, scope);
    Check(unavailable.requested && unavailable.references == std::vector{variation},
        "The actual forgotten-version request lost its unavailable identity or fell back to the original.");
    const auto last = SelectConversationImages("Describe the generated variation you just used in the comparison.", history, scope);
    Check(last.requested && last.references == std::vector{variation},
        "The actual generated-variation request escaped native source lookup.");
    Check(SelectConversationImages("Describe the previous generated version.", history, scope).references == std::vector{original},
        "An explicit previous generated version selected the latest variation.");
    const auto third = SelectConversationImages("Describe the third generated version.", history, scope);
    Check(third.requested && third.references.empty() && !third.notice.empty(), "An unavailable generated ordinal guessed a source.");
    for (const std::string question : {"Explain generated version control.", "Explain the generated version of this C++ function.",
             "Read the phrase 'first generated version'.", "Describe the words \"generated variation\"."})
        Check(!SelectConversationImages(question, history, scope).requested,
            "A quoted or software-version concept acquired private generated pixels: " + question);
    for (int changed = 0; changed < 3; ++changed)
    {
        auto foreign = scope;
        if (changed == 0)
            foreign.companionId = "other";
        if (changed == 1)
            foreign.participantId = "other";
        if (changed == 2)
            foreign.audience.kind = revia::identity::AudienceKind::Public;
        const auto denied = SelectConversationImages("Describe the first generated version.", history, foreign);
        Check(denied.requested && denied.references.empty() && !denied.notice.empty(), "Generated version selection crossed scope.");
    }
    auto generic = first;
    generic.mediaReferences.front().origin = MediaOrigin::UserAttachment;
    Check(SelectConversationImages("Describe the generated variation.", {generic}, scope).references.empty(),
        "Generic assistant media became generated-version authority.");
    conversationMediaReference uploadOne{"upload-one", std::string(64, 'c'), "upload-one.png", "image/png", 30, 20};
    auto uploadTwo = uploadOne;
    uploadTwo.id = "upload-two";
    uploadTwo.name = "upload-two.png";
    conversationMessage upload{"user", "Inspect these two images.", scope.participantId, scope};
    upload.mediaReferences = {uploadOne, uploadTwo};
    Check(SelectConversationImages("Describe the second image.", {first, second, upload}, scope).references == std::vector{uploadTwo},
        "Generated version chronology changed same-message upload ordinals.");
    const auto ambiguousCommand = SelectConversationImages("/imagine using the previous image", {upload}, scope);
    Check(ambiguousCommand.requested && ambiguousCommand.references.empty() && !ambiguousCommand.notice.empty(),
        "An imagine command guessed between two images in the same upload group.");
    upload.mediaReferences = {uploadOne};
    Check(SelectConversationImages("/imagine using the previous image", {upload}, scope).references == std::vector{uploadOne},
        "An explicit imagine command dropped its previous-image reference.");
    auto newer = upload;
    newer.mediaReferences = {uploadTwo};
    Check(SelectConversationImages("/imagine using the previous image", {upload, newer}, scope).references == std::vector{uploadOne},
        "An explicit imagine command lost the previous chronological image.");
    upload.mediaReferences.front().name = "the first generated version.png";
    Check(SelectConversationImages("Describe the first generated version.png.", {upload}, scope).references == upload.mediaReferences,
        "Generated-version words inside a literal upload filename changed its origin or ordinal.");
}

void TestScopedImageSelection()
{
    revia::memory::MemoryScope scope;
    scope.companionId = "companion";
    scope.participantId = "local:user";
    scope.audience = {revia::identity::AudienceKind::Private, "private", 4, {}};
    conversationMediaReference first{"one", std::string(64, 'a'), "001.png", "image/png", 30, 20};
    conversationMediaReference second{"two", std::string(64, 'b'), "002.png", "image/png", 30, 20};
    conversationMessage message{"user", "Compare these.", scope.participantId, scope};
    message.mediaReferences = {first, second};
    const auto selected = revia::attachments::SelectConversationImages("What about the second image?", {message}, scope);
    Check(selected.requested && selected.references == std::vector{second}, "The second image did not retain its original order.");
    Check(revia::attachments::SelectConversationImages("Compare them.", {message}, scope).references == std::vector{first, second},
        "Comparison did not retain both original image references.");
    Check(revia::attachments::SelectConversationImages("Compare the printed labels on the first and second images.", {message}, scope)
                  .references == std::vector{first, second},
        "An explicitly numbered comparison silently selected only its first image.");
    Check(revia::attachments::SelectConversationImages("Compare 001.png with the second image.", {message}, scope).references ==
              std::vector{first, second},
        "A mixed filename-and-ordinal comparison omitted one explicit reference.");
    Check(revia::attachments::SelectConversationImages("Describe 001.png and the second image.", {message}, scope).references.empty(),
        "Conflicting explicit image identifiers were silently narrowed.");
    for (const std::string input : {"Describe image 2.", "Describe image#2.", "Describe image2."})
        Check(revia::attachments::SelectConversationImages(input, {message}, scope).references == std::vector{second},
            "A noun-qualified numeric image identifier lost its ordinal.");
    const auto ambiguous = revia::attachments::SelectConversationImages("Which part of it?", {message}, scope);
    Check(ambiguous.requested && ambiguous.references.empty() && !ambiguous.notice.empty(), "An ambiguous image follow-up guessed.");
    Check(!revia::attachments::SelectConversationImages("How are you today?", {message}, scope).requested,
        "Unrelated conversation inherited images.");
    Check(!revia::attachments::SelectConversationImages("Describe how linking works.", {message}, scope).requested,
        "An unrelated explanation request inherited images.");
    Check(!revia::attachments::SelectConversationImages("Which part?", {}, scope).requested,
        "A context-free follow-up was reinterpreted as an image request.");
    auto narrowed = message;
    narrowed.mediaReferences = {second};
    Check(revia::attachments::SelectConversationImages("What about the first image?", {message, narrowed}, scope).references ==
              std::vector{first},
        "Reinspection narrowed the original upload's image numbering.");
    Check(revia::attachments::SelectConversationImages("What about the second image?", {message, narrowed}, scope).references ==
              std::vector{second},
        "Repeated second-image selection lost its original ordinal.");
    Check(revia::attachments::SelectConversationImages("Which part?", {message, narrowed}, scope).references == std::vector{second},
        "A unique recent image focus was lost after reinspection.");
    Check(revia::attachments::SelectConversationImages("Which distractions would you remove?", {message, narrowed}, scope).references ==
              std::vector{second},
        "An image critique lost the unique recent image focus.");
    const conversationMessage unrelated{"user", "Explain linking.", scope.participantId, scope};
    Check(!revia::attachments::SelectConversationImages("Which part?", {message, unrelated}, scope).requested,
        "An unrelated intervening topic inherited a stale implicit image focus.");
    Check(
        revia::attachments::SelectConversationImages("Compare them.", {message, narrowed}, scope).references == std::vector{first, second},
        "Reinspection changed the original comparison group.");
    for (int changed = 0; changed < 4; ++changed)
    {
        auto foreign = scope;
        if (changed == 0)
            foreign.companionId = "other";
        if (changed == 1)
            foreign.participantId = "other";
        if (changed == 2)
            foreign.audience.audienceId = "other";
        if (changed == 3)
            foreign.audience.kind = revia::identity::AudienceKind::Shared;
        Check(revia::attachments::SelectConversationImages("Describe the image.", {message}, foreign).references.empty(),
            "A different companion, participant or audience inherited image references.");
    }
    message.mediaReferences = {first};
    Check(revia::attachments::SelectConversationImages("Which part?", {message}, scope).references == std::vector{first},
        "A unique image follow-up lost its reference.");
    auto newer = message;
    newer.mediaReferences = {second};
    Check(revia::attachments::SelectConversationImages("Compare it with the previous version.", {message, newer}, scope).references ==
              std::vector{first, second},
        "A previous-version comparison lost chronological identity.");
    Check(revia::attachments::SelectConversationImages(
              "Read the final return statement from the previous version of the code image.", {message, newer}, scope)
                  .references == std::vector{first},
        "A qualified code-image previous-version request lost its earlier image.");
    Check(revia::attachments::SelectConversationImages("Show the previous version.", {message, newer}, scope).references ==
              std::vector{first},
        "A recent-media previous-version reference was overwritten by the newest image focus.");
    Check(!revia::attachments::SelectConversationImages("Show the previous version.", {message, unrelated}, scope).requested,
        "A code-topic previous-version request silently inherited a stale picture.");
    Check(
        revia::attachments::SelectConversationImages("Compare it with the previous version.", {message, newer, newer}, scope).references ==
            std::vector{first, second},
        "Reinspection created a duplicate version for comparison.");
    newer.mediaReferences.front().name = first.name;
    Check(revia::attachments::SelectConversationImages("Describe 001.png.", {message, newer}, scope).references.empty(),
        "Duplicate image names silently selected several images.");
    conversationContext retained;
    retained.AddMessage(message);
    retained.AddMessage("assistant", "sentinel_private_image_analysis");
    retained.InvalidateMediaAnalysis(first.id);
    Check(retained.GetRecentMessages().back().content.find("sentinel_private_image_analysis") == std::string::npos &&
              retained.GetCompressedHistorySummary().find("sentinel_private_image_analysis") == std::string::npos,
        "Forgetting media left its derived analysis in working history or continuity.");
    conversationContext longHistory;
    longHistory.AddMessage("user", "We decided the release must retain sentinel_unrelated_decision.");
    longHistory.AddMessage(message);
    longHistory.AddMessage("assistant", "The image must retain sentinel_private_image_analysis.");
    for (int index = 0; index < 30; ++index)
        longHistory.AddMessage(index % 2 == 0 ? "user" : "assistant", "A routine unrelated exchange.");
    Check(longHistory.GetCompressedHistorySummary().find("sentinel_unrelated_decision") != std::string::npos,
        "The continuity fixture did not retain its trimmed decision.");
    longHistory.InvalidateMediaAnalysis(first.id);
    Check(longHistory.GetCompressedHistorySummary().find("sentinel_unrelated_decision") != std::string::npos &&
              longHistory.GetCompressedHistorySummary().find("sentinel_private_image_analysis") == std::string::npos,
        "Image forgetting erased an unrelated trimmed decision or retained derived image analysis.");
    Check(revia::attachments::SelectConversationImages("What about the third image?", {message}, scope).references.empty(),
        "An unavailable ordinal silently selected another image.");
}

void TestLiteralImageNames()
{
    revia::memory::MemoryScope scope;
    scope.companionId = "companion";
    scope.participantId = "local:user";
    scope.audience = {revia::identity::AudienceKind::Private, "private", 4, {}};
    conversationMediaReference picture{"one", std::string(64, 'a'), "\xE7\x8B\x97.png", "image/png", 30, 20};
    conversationMessage message{"user", "Describe the attachment.", scope.participantId, scope};
    message.mediaReferences = {picture};
    Check(revia::attachments::SelectConversationImages("Describe \xE7\x8C\xAB.png.", {message}, scope).references.empty(),
        "A different UTF-8 filename selected an unrelated image.");
    Check(revia::attachments::SelectConversationImages("Describe the image \xE7\x8C\xAB.png.", {message}, scope).references.empty(),
        "An unavailable explicit UTF-8 filename fell back to the only image.");
    Check(revia::attachments::SelectConversationImages("Describe `" + picture.name + "`.", {message}, scope).references ==
              std::vector{picture},
        "An exact UTF-8 filename did not retain its attachment identity.");
    picture.name = "photo-1.png";
    message.mediaReferences = {picture};
    for (const std::string name : {"photo_1.png", "xphoto-1.png", "photo-1.png.backup"})
        Check(revia::attachments::SelectConversationImages("Describe the image " + name + ".", {message}, scope).references.empty(),
            "A filename with different punctuation or a longer token selected an unrelated image.");
    Check(revia::attachments::SelectConversationImages("Describe PHOTO-1.PNG.", {message}, scope).references == std::vector{picture},
        "ASCII case folding did not match an otherwise exact filename.");
    Check(revia::attachments::SelectConversationImages("Compare photo-1.png with photo_1.png.", {message}, scope).references.empty(),
        "A comparison silently omitted an explicitly named unavailable image.");
    Check(!revia::attachments::SelectConversationImages("What is a .png file?", {message}, scope).requested,
        "A question about a format extension was mistaken for a named attachment.");
    picture.name = "image2.png";
    message.mediaReferences = {picture};
    Check(revia::attachments::SelectConversationImages("Describe image2.png.", {message}, scope).references == std::vector{picture},
        "A filename's numeric-looking text was reinterpreted as a different ordinal.");
    picture.name = "first.png";
    auto earlier = picture;
    earlier.id = "earlier";
    earlier.name = "other.png";
    message.mediaReferences = {earlier, picture};
    Check(revia::attachments::SelectConversationImages("Describe first.png.", {message}, scope).references == std::vector{picture},
        "A filename's ordinal-looking text replaced its literal identity.");
    picture.name = "compare.png";
    auto duplicate = picture;
    duplicate.id = "two";
    message.mediaReferences = {picture, duplicate};
    Check(revia::attachments::SelectConversationImages("Describe compare.png.", {message}, scope).references.empty(),
        "A filename's comparison-looking text silently selected duplicate names.");
}

void TestImageReferenceCues()
{
    revia::memory::MemoryScope scope;
    scope.companionId = "companion";
    scope.participantId = "local:user";
    scope.audience = {revia::identity::AudienceKind::Private, "private", 4, {}};
    conversationMediaReference picture{"one", std::string(64, 'a'), "001.png", "image/png", 30, 20};
    conversationMessage message{"user", "Describe the attachment.", scope.participantId, scope};
    message.mediaReferences = {picture};
    for (const std::string input :
        {"How does image compression work?", "Tell me a story about a picture book.", "Draw a new picture of a cat.",
            "Explain the image compression algorithm.", "What color is the sky?", "How would you edit a screenplay?"})
        Check(!revia::attachments::SelectConversationImages(input, {message}, scope).requested,
            "An unrelated image concept, creative request or question inherited a private image.");
    for (const std::string input : {"Describe the image.", "What about the first image?", "Which part?", "Draw a picture based on 001.png.",
             "a variation of 001.png", "a version of the first image", "Draw a picture based on this.", "Turn it into a watercolor.",
             "Convert it into a watercolor.", "Redraw it in blue.", "Read the short label from that still image."})
    {
        const auto selected = revia::attachments::SelectConversationImages(input, {message}, scope);
        Check(selected.requested && selected.references == std::vector{picture},
            "A definite, named, ordinal or contextual image reference was missed.");
    }
}

void TestAfterNounUploadReferencesKeepAvailabilityAndScope()
{
    revia::memory::MemoryScope scope;
    scope.companionId = "companion";
    scope.participantId = "local:user";
    scope.audience = {revia::identity::AudienceKind::Private, "private", 4, {}};
    conversationMediaReference picture{"one", std::string(64, 'a'), "001.png", "image/png", 30, 20};
    conversationMessage upload{"user", "Inspect this picture.", scope.participantId, scope};
    upload.mediaReferences = {picture};
    for (const std::string input :
        {"What picture did I upload in this new conversation?", "What image did I attach?", "What photo did I just upload?",
            "What picture did I just attach?", "Describe an image I just uploaded.", "Describe a picture I just attached."})
    {
        const auto empty = revia::attachments::SelectConversationImages(input, {}, scope);
        Check(empty.requested && empty.references.empty() && !empty.notice.empty(),
            "An unavailable after-noun upload reference reached ordinary image guessing: " + input);
        const auto available = revia::attachments::SelectConversationImages(input, {upload}, scope);
        Check(available.requested && available.references == std::vector{picture},
            "An after-noun upload reference lost its available scoped image: " + input);
    }
    const std::string question = "What picture did I upload in this new conversation?";
    auto several = upload;
    auto second = picture;
    second.id = "two";
    second.name = "002.png";
    several.mediaReferences.push_back(second);
    const auto ambiguous = revia::attachments::SelectConversationImages(question, {several}, scope);
    Check(ambiguous.requested && ambiguous.references.empty() && !ambiguous.notice.empty(),
        "An ambiguous upload reference guessed which available image was intended.");
    for (int changed = 0; changed < 3; ++changed)
    {
        auto current = scope;
        if (changed == 0)
            current.companionId = "other";
        if (changed == 1)
            current.participantId = "other";
        if (changed == 2)
            current.audience.kind = revia::identity::AudienceKind::Public;
        const auto withheld = revia::attachments::SelectConversationImages(question, {upload}, current);
        Check(withheld.requested && withheld.references.empty() && !withheld.notice.empty(),
            "An after-noun upload reference crossed its companion, participant or audience scope.");
    }
}

void TestImageAttachmentCountsDoNotCountVisualObjectsOrSubsets()
{
    Check(!revia::attachments::RequestsImageAttachmentCount("How many pictures did I upload?"),
        "An original upload-ledger count was confused with the admitted image count for this request.");
    for (const std::string input :
        {"How many separate pictures can you actually inspect visually in this message?", "How many attached images can you inspect?",
            "How many uploaded pictures are available to inspect?", "How many separate photos are included in this request?",
            "How many images did you receive in this request?", "How many pictures are available in this message?",
            "How many photos can you inspect in this message?", "How many uploaded photos can you actually inspect visually?",
            "How many separate images in this message?", "How many images are available to inspect?",
            "How many pictures are available in this request?", "  HOW many ATTACHED images can you inspect?\t"})
        Check(revia::attachments::RequestsImageAttachmentCount(input),
            "A pure attached-image availability/count question was not recognized: " + input);
    for (const std::string input :
        {"How many circles are in the image?", "How many pictures contain red?", "How many attached images?",
            "How many images are attached?", "How many uploaded pictures are there?", "How many pictures did I upload in this message?",
            "How many images have I attached?", "How many attached images have red circles?", "How many of the attached images are photos?",
            "How many images are attached if one is missing?", "If two are unreadable, how many pictures can you inspect?",
            "\"How many attached images?\"", "'How many attached images?'", "`How many attached images?`", "> How many attached images?",
            "Explain how many images a PNG can contain.", "How many images does compression support?",
            "How many red pictures are attached?", "How many separate pictures are in the image?", "How many images?",
            "How many images can you see?", "How many attached images, and what do they show?"})
        Check(!revia::attachments::RequestsImageAttachmentCount(input),
            "A visual-content, subset, conditional, conceptual or quoted question was treated as an attachment count: " + input);

    revia::memory::MemoryScope scope;
    scope.companionId = "companion";
    scope.participantId = "local:user";
    scope.audience = {revia::identity::AudienceKind::Private, "private", 4, {}};
    conversationMediaReference first{"one", std::string(64, 'a'), "001.png", "image/png", 30, 20};
    conversationMediaReference second{"two", std::string(64, 'b'), "002.png", "image/png", 30, 20};
    conversationMessage upload{"user", "Inspect these pictures.", scope.participantId, scope};
    conversationMediaReference third{"three", std::string(64, 'c'), "003.png", "image/png", 30, 20};
    conversationMediaReference fourth{"four", std::string(64, 'd'), "004.png", "image/png", 30, 20};
    upload.mediaReferences = {first, second, third, fourth};
    auto focused = upload;
    focused.mediaReferences = {second};
    const std::string count = "How many separate photos are in this message?";
    const auto selected = revia::attachments::SelectConversationImages(count, {upload, focused}, scope);
    Check(selected.requested && selected.references == std::vector{first, second, third, fourth},
        "A pure availability count inherited one image focus instead of the relevant upload group.");
    const std::string contentQuestion = "Compare the first, second, third and fourth images. For the first and second, give the "
                                        "upper-right shape and its color. For the third, read the final return value. For the fourth, "
                                        "give the roof color and window count.";
    Check(!revia::attachments::RequestsImageAttachmentCount(contentQuestion) &&
              revia::attachments::SelectConversationImages(contentQuestion, {upload, focused}, scope).references ==
                  std::vector{first, second, third, fourth},
        "A four-source visual-content comparison became an attachment-count answer or lost its distinct references.");
    Check(!revia::attachments::SelectConversationImages("How does image compression work?", {upload}, scope).requested,
        "An image concept inherited image pixels through the count route.");
    for (int changed = 0; changed < 3; ++changed)
    {
        auto current = scope;
        if (changed == 0)
            current.companionId = "other";
        if (changed == 1)
            current.participantId = "other";
        if (changed == 2)
            current.audience.kind = revia::identity::AudienceKind::Public;
        const auto withheld = revia::attachments::SelectConversationImages(count, {upload}, current);
        Check(withheld.requested && withheld.references.empty(), "A count request crossed its companion, participant or audience scope.");
    }
    const auto empty = revia::attachments::SelectConversationImages(count, {}, scope);
    Check(empty.requested && empty.references.empty(), "An empty conversation acquired image associations from a count request.");
}

std::vector<conversationMessage> ImageSelectionHistory(const conversationContext& context)
{
    auto history = context.GetMediaHistory();
    const auto recent = context.GetRecentMessages();
    history.insert(history.end(), recent.begin(), recent.end());
    return history;
}

void TestImageMetadataSurvivesTextTrimming()
{
    revia::memory::MemoryScope scope;
    scope.companionId = "companion";
    scope.participantId = "local:user";
    scope.audience = {revia::identity::AudienceKind::Private, "private", 4, {}};
    conversationMediaReference picture{"one", std::string(64, 'a'), "001.png", "image/png", 30, 20};
    conversationMessage upload{"user", "sentinel_original_upload_text", scope.participantId, scope};
    upload.mediaReferences = {picture};
    upload.images.push_back({"001.png", "image/png", std::make_shared<const std::string>("sentinel_pixel_bytes"), 30, 20});
    conversationContext context;
    context.SetContextTokens(2048);
    context.AddMessage(upload);
    std::vector<conversationMessage> restored = {upload};
    for (int index = 0; index < 40; ++index)
    {
        conversationMessage ordinary{index % 2 == 0 ? "assistant" : "user", "An unrelated ordinary exchange.", scope.participantId, scope};
        context.AddMessage(ordinary);
        restored.push_back(ordinary);
    }
    const auto recent = context.GetRecentMessages();
    Check(std::none_of(recent.begin(), recent.end(), [](const auto& message) { return !message.mediaReferences.empty(); }),
        "The text-trimming fixture retained its original image-bearing turn.");
    Check(revia::attachments::SelectConversationImages("Describe 001.png.", ImageSelectionHistory(context), scope).references ==
              std::vector{picture},
        "Text-budget trimming discarded a still-authorized stored image association.");
    Check(!revia::attachments::SelectConversationImages("Which part?", ImageSelectionHistory(context), scope).requested,
        "Old image metadata overrode an unrelated latest topic's implicit focus.");
    const auto metadata = context.GetMediaHistory();
    Check(metadata.size() == 1 && metadata.front().content.empty() && metadata.front().images.empty() &&
              metadata.front().memoryScope.has_value() && metadata.front().mediaReferences == std::vector{picture},
        "Retained attachment selection metadata included dialogue, pixel bytes or omitted its scope.");
    context.InvalidateMediaAnalysis(picture.id);
    Check(context.GetMediaHistory().front().mediaReferences == std::vector{picture},
        "Forgetting an image lost its unavailable ordinal tombstone or changed its archive-valid identity.");
    context.Clear();
    Check(context.GetMediaHistory().empty() &&
              revia::attachments::SelectConversationImages("Describe 001.png.", ImageSelectionHistory(context), scope).references.empty(),
        "A fresh conversation inherited a previous image association.");
    context.RestoreMessages(restored, 4);
    Check(revia::attachments::SelectConversationImages("Describe 001.png.", ImageSelectionHistory(context), scope).references ==
              std::vector{picture},
        "Scoped archive restoration lost metadata from an image turn outside its recent text tail.");
    context.Clear();
    context.AddMessage(upload);
    Check(!context.RemoveLastMessageIf("user", "different text") && context.GetMediaHistory().size() == 1,
        "An inexact retraction removed another source's image association.");
    Check(context.RemoveLastMessageIf("user", upload.content) && context.GetMediaHistory().empty(),
        "An exact user retraction left a ghost image association.");
    for (int index = 0; index < 300; ++index)
    {
        upload.mediaReferences.front().id = "picture-" + std::to_string(index);
        context.AddMessage(upload);
    }
    Check(context.GetMediaHistory().size() == 256, "Attachment metadata was not bounded independently of the model text window.");
}
}

void RunMediaSelectionTests()
{
    TestGeneratedVersionAliasesRetainChronologyAndUnavailableIdentity();
    Check(revia::attachments::RequestsImageTransformation("Change this image to blue."), "An explicit image edit was missed.");
    Check(revia::attachments::RequestsImageTransformation("Could you please animate this image?"),
        "An explicit animation request was missed.");
    Check(!revia::attachments::RequestsImageTransformation("What would you change about this picture?") &&
              !revia::attachments::RequestsImageTransformation("Which distractions would you remove?"),
        "Image critique was mistaken for a request to produce an edit.");
    TestLiteralImageNames();
    TestImageReferenceCues();
    TestAfterNounUploadReferencesKeepAvailabilityAndScope();
    TestImageAttachmentCountsDoNotCountVisualObjectsOrSubsets();
    TestImageMetadataSurvivesTextTrimming();
    TestScopedImageSelection();
    std::cout << "Image follow-ups preserve scoped references and refuse ambiguous selection.\n";
}
