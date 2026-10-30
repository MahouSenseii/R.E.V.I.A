#include "Attachments/mediaSelection.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <iterator>
#include <string_view>

namespace revia::attachments
{
namespace
{
std::string Words(const std::string& input)
{
    std::string result = " ";
    for (const unsigned char byte : input)
        if (std::isalnum(byte))
            result += static_cast<char>(std::tolower(byte));
        else if (result.back() != ' ')
            result += ' ';
    if (result.back() != ' ')
        result += ' ';
    return result;
}

bool SameScope(const memory::MemoryScope& stored, const memory::MemoryScope& current)
{
    return current.audience.kind == identity::AudienceKind::Private && stored.audience.kind == identity::AudienceKind::Private &&
           !current.companionId.empty() && stored.companionId == current.companionId && !current.participantId.empty() &&
           stored.participantId == current.participantId && !current.audience.audienceId.empty() &&
           stored.audience.audienceId == current.audience.audienceId && stored.participantSource == current.participantSource &&
           (current.participantSource != identity::SpeakerSource::ConsentedVoice ||
               (current.consentRevision != 0 && stored.consentRevision == current.consentRevision));
}

bool Contains(const std::string& text, const std::initializer_list<std::string_view> terms)
{
    return std::any_of(terms.begin(), terms.end(), [&](const auto term) { return text.find(term) != std::string::npos; });
}

std::string OriginWords(const std::string& input)
{
    std::string visible;
    char quote = 0;
    for (std::size_t at = 0; at < input.size(); ++at)
    {
        const char byte = input[at];
        if (quote)
        {
            if (byte == '\\' && at + 1 < input.size())
                ++at;
            else if (byte == quote)
                quote = 0;
            continue;
        }
        const bool contraction = byte == '\'' && at > 0 && at + 1 < input.size() &&
                                 std::isalnum(static_cast<unsigned char>(input[at - 1])) &&
                                 std::isalnum(static_cast<unsigned char>(input[at + 1]));
        if ((byte == '\'' && !contraction) || byte == '"' || byte == '`')
        {
            quote = byte;
            visible += ' ';
        }
        else
            visible += byte;
    }
    return Words(visible);
}

bool NamesOrigin(const std::string& words, const bool generated)
{
    for (const std::string noun : {"image", "images", "picture", "pictures", "photo", "photos", "attachment", "attachments"})
    {
        const auto marker = " " + noun + " ";
        if (generated ? words.find(" generated" + marker) != std::string::npos
                      : words.find(" uploaded" + marker) != std::string::npos || words.find(" attached" + marker) != std::string::npos)
            return true;
        for (const auto suffix : generated
                                     ? std::initializer_list<std::string_view>{"you generated ", "you made ", "you created ", "you drew "}
                                     : std::initializer_list<std::string_view>{"i uploaded ", "i attached ", "i just uploaded ",
                                           "i just attached ", "did i upload ", "did i attach "})
            if (words.find(marker + std::string(suffix)) != std::string::npos)
                return true;
    }
    return Contains(words, {" generated and uploaded ", " uploaded and generated "});
}

bool GeneratedVersionReference(const std::string& words)
{
    for (const std::string noun : {"version", "versions", "variation", "variations"})
    {
        const auto marker = " generated " + noun + " ";
        for (auto at = words.find(marker); at != std::string::npos; at = words.find(marker, at + 1))
        {
            const std::string_view before(words.data(), at + 1);
            const auto after = std::string_view(words).substr(at + marker.size() - 1);
            if (after.starts_with(" of ") || after.starts_with(" control "))
                continue;
            for (const std::string_view qualifier : {" the ", " this ", " that ", " these ", " those ", " first ", " second ", " third ",
                     " fourth ", " previous ", " last ", " latest ", " same "})
                if (before.ends_with(qualifier))
                    return true;
        }
    }
    return false;
}

bool GeneratedVersionOrdinal(const std::string& words, const std::size_t index)
{
    const std::string ordinals[] = {"first", "second", "third", "fourth"};
    for (const std::string noun : {"version", "versions", "variation", "variations"})
        if (words.find(" " + ordinals[index] + " generated " + noun + " ") != std::string::npos)
            return true;
    return false;
}

std::string FoldAscii(std::string input)
{
    for (char& byte : input)
        if (byte >= 'A' && byte <= 'Z')
            byte += 'a' - 'A';
    return input;
}

bool FilenameContinuation(const unsigned char byte)
{
    return byte >= 0x80 || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.' ||
           byte == '/' || byte == '\\';
}

bool FilenameEnd(const std::string& input, std::size_t position)
{
    if (position == input.size() || !FilenameContinuation(static_cast<unsigned char>(input[position])))
        return true;
    if (input[position] != '.')
        return false;
    while (position < input.size() && input[position] == '.')
        ++position;
    return position == input.size() || !FilenameContinuation(static_cast<unsigned char>(input[position]));
}

bool MarkLiteralName(const std::string& input, const std::string& name, std::vector<bool>& matched)
{
    if (name.size() < 4)
        return false;
    bool found = false;
    const auto foldedName = FoldAscii(name);
    for (auto position = input.find(foldedName); position != std::string::npos; position = input.find(foldedName, position + 1))
        if ((position == 0 || !FilenameContinuation(static_cast<unsigned char>(input[position - 1]))) &&
            FilenameEnd(input, position + foldedName.size()))
        {
            std::fill(matched.begin() + position, matched.begin() + position + foldedName.size(), true);
            found = true;
        }
    return found;
}

bool HasUnmatchedImageName(const std::string& input, const std::vector<bool>& matched)
{
    for (const std::string_view extension : {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"})
        for (auto position = input.find(extension); position != std::string::npos; position = input.find(extension, position + 1))
            if (position > 0 && FilenameContinuation(static_cast<unsigned char>(input[position - 1])) &&
                std::find(matched.begin() + position, matched.begin() + position + extension.size(), false) !=
                    matched.begin() + position + extension.size())
                return true;
    return false;
}

bool HasImageReferenceNoun(const std::string& words, const bool requireQualifier = true)
{
    for (const std::string_view noun : {" image ", " images ", " picture ", " pictures ", " photo ", " photos ", " screenshot ",
             " screenshots ", " attachment ", " attachments "})
        for (auto position = words.find(noun); position != std::string::npos; position = words.find(noun, position + 1))
        {
            std::string_view before(words.data(), position + 1);
            const std::string_view after(words.data() + position + noun.size() - 1, words.size() - position - noun.size() + 1);
            bool compoundNoun = false;
            for (const std::string_view next : {" compression ", " processing ", " generation ", " recognition ", " format ", " formats ",
                     " book ", " books ", " model ", " models "})
                compoundNoun = compoundNoun || after.starts_with(next);
            if (compoundNoun)
                continue;
            if (!requireQualifier)
                return true;
            for (int count = 0; count < 2; ++count)
                for (const std::string_view modifier : {" same ", " still ", " new ", " code ", " two ", " three ", " four "})
                    if (before.ends_with(modifier))
                    {
                        before.remove_suffix(modifier.size() - 1);
                        break;
                    }
            for (const std::string_view qualifier :
                {" the ", " this ", " that ", " these ", " those ", " my ", " your ", " our ", " attached ", " uploaded ", " previous ",
                    " earlier ", " last ", " same ", " reference ", " generated ", " first ", " second ", " third ", " fourth ", " which "})
                if (before.ends_with(qualifier))
                    return true;
            for (const std::string_view uploadReference : {" i attached ", " i uploaded ", " i just attached ", " i just uploaded ",
                     " did i attach ", " did i upload ", " did i just attach ", " did i just upload ", " from earlier ",
                     " you generated ", " you made ", " you created ", " you drew "})
                if (after.starts_with(uploadReference))
                    return true;
        }
    return false;
}

bool ShortImageFollowup(const std::string& words)
{
    return Contains(words,
               {" compare it ", " compare them ", " compare these ", " compare both ", " describe it ", " describe them ",
                   " can you see it ", " edit it ", " animate it ", " make it ", " change it ", " turn it ", " convert it ", " redraw it ",
                   " what colour is it ", " what color is it ", " how would you edit it ", " what would you change about it "}) ||
           words.ends_with(" which part ") || words.ends_with(" which part of it ") || words.ends_with(" describe that ") ||
           words.ends_with(" look again ") || words.ends_with(" what colour ") || words.ends_with(" what color ") ||
           words.ends_with(" which distractions would you remove ") || words.ends_with(" what would you change ") ||
           words.ends_with(" based on this ") || words.ends_with(" based on that ") || words.ends_with(" using this ") ||
           words.ends_with(" using that ");
}

bool NumberedImage(const std::string& words, const std::size_t index)
{
    const auto number = std::to_string(index + 1);
    for (const std::string_view noun : {"image", "picture", "photo", "screenshot", "attachment"})
        if (words.find(" " + std::string(noun) + " " + number + " ") != std::string::npos ||
            words.find(" " + std::string(noun) + number + " ") != std::string::npos)
            return true;
    return false;
}
}

bool RequestsImageTransformation(const std::string& input)
{
    std::string action = Words(input).substr(1);
    for (const std::string_view prefix : {"could you please ", "can you please ", "could you ", "can you ", "would you ", "will you ",
             "please ", "i want you to ", "i d like you to "})
        if (action.starts_with(prefix))
        {
            action.erase(0, prefix.size());
            break;
        }
    for (const std::string_view verb : {"edit ", "change ", "modify ", "remove ", "replace ", "animate ", "redraw ", "make it ",
             "turn this ", "turn it ", "convert this ", "convert it "})
        if (action.starts_with(verb))
            return true;
    return false;
}

bool RequestsImageAttachmentCount(const std::string& input)
{
    if (input.empty() || input.size() > 256)
        return false;
    const auto first = input.find_first_not_of(" \t\r\n");
    const auto last = input.find_last_not_of(" \t\r\n");
    if (first == std::string::npos)
        return false;
    auto end = last + 1;
    if (input[end - 1] == '?' || input[end - 1] == '.')
        --end;
    if (end <= first)
        return false;
    for (std::size_t index = first; index < end; ++index)
    {
        const auto byte = static_cast<unsigned char>(input[index]);
        if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || std::isspace(byte)))
            return false;
    }
    const auto normalized = Words(input.substr(first, end - first));
    std::string_view remaining(normalized);
    constexpr std::string_view prefix = " how many ";
    if (!remaining.starts_with(prefix))
        return false;
    remaining.remove_prefix(prefix.size());
    bool sourceQualified = false;
    for (int count = 0; count < 3; ++count)
    {
        bool removed = false;
        for (const std::string_view qualifier : {"attached ", "uploaded ", "separate "})
            if (remaining.starts_with(qualifier))
            {
                sourceQualified = sourceQualified || qualifier != "separate ";
                remaining.remove_prefix(qualifier.size());
                removed = true;
                break;
            }
        if (!removed)
            break;
    }
    bool imageNoun = false;
    for (const std::string_view noun : {"images ", "pictures ", "photos "})
        if (remaining.starts_with(noun))
        {
            remaining.remove_prefix(noun.size() - 1);
            imageNoun = true;
            break;
        }
    if (!imageNoun)
        return false;
    bool thisRequest = false;
    for (const std::string_view suffix : {" in this message ", " in this request "})
        if (remaining.ends_with(suffix))
        {
            remaining.remove_suffix(suffix.size() - 1);
            thisRequest = true;
            break;
        }
    for (const std::string_view tail : {" are available ", " are available to inspect ", " are inspectable "})
        if (remaining == tail)
            return true;
    if (thisRequest)
        for (const std::string_view tail :
            {" ", " are ", " are there ", " are included ", " are received ", " did you receive ", " have you received "})
            if (remaining == tail)
                return true;
    if (sourceQualified || thisRequest)
        for (const std::string_view tail : {" are visible ", " can you see ", " can you inspect ", " can you view ",
                 " can you actually see ", " can you actually inspect ", " can you actually view ", " can you inspect visually ",
                 " can you actually inspect visually "})
            if (remaining == tail)
                return true;
    return false;
}

MediaSelection SelectConversationImages(
    const std::string& input, const std::vector<conversationMessage>& history, const memory::MemoryScope& scope)
{
    MediaSelection result;
    const auto words = Words(input);
    const auto authoredWords = OriginWords(input);
    const bool generatedVersionCue = GeneratedVersionReference(authoredWords);
    const bool attachmentCount = RequestsImageAttachmentCount(input);
    const bool previousVersion = words.find(" previous version ") != std::string::npos ||
                                 (generatedVersionCue && authoredWords.find(" previous generated ") != std::string::npos);
    const bool explicitImage = generatedVersionCue || attachmentCount || HasImageReferenceNoun(words) ||
                               (previousVersion && HasImageReferenceNoun(words, false)) || NumberedImage(words, 0) ||
                               NumberedImage(words, 1) || NumberedImage(words, 2) || NumberedImage(words, 3);
    const bool followUp = input.size() <= 180 && ShortImageFollowup(words);
    std::vector<std::vector<conversationMediaReference>> groups;
    std::vector<conversationMediaReference> focus;
    std::vector<std::string> knownIds;
    bool latestHasMedia = false;
    for (const auto& message : history)
    {
        const bool generated = message.role == "assistant" && memory::IsAttributedPrivateScope(scope) &&
                               std::all_of(message.mediaReferences.begin(), message.mediaReferences.end(),
                                   [](const auto& reference) { return reference.origin == MediaOrigin::GeneratedArtifact; });
        const bool scoped = message.memoryScope && SameScope(*message.memoryScope, scope);
        const bool hasImages = scoped && std::any_of(message.mediaReferences.begin(), message.mediaReferences.end(),
                                             [](const auto& reference) { return reference.mimeType.starts_with("image/"); });
        if (message.role == "user")
            latestHasMedia = hasImages;
        else if (generated && !message.mediaReferences.empty())
            latestHasMedia = hasImages;
        if ((message.role == "user" || generated) && hasImages)
        {
            focus.clear();
            for (const auto& reference : message.mediaReferences)
                if (reference.mimeType.starts_with("image/"))
                    focus.push_back(reference);
            std::vector<conversationMediaReference> uploaded;
            for (const auto& reference : focus)
                if (std::find(knownIds.begin(), knownIds.end(), reference.id) == knownIds.end())
                {
                    knownIds.push_back(reference.id);
                    uploaded.push_back(reference);
                }
            if (!uploaded.empty())
                groups.push_back(std::move(uploaded));
        }
    }
    std::vector<conversationMediaReference> named;
    const auto literalInput = FoldAscii(input);
    std::vector<bool> matchedNameBytes(input.size(), false);
    for (const auto& group : groups)
        for (const auto& reference : group)
            if (MarkLiteralName(literalInput, reference.name, matchedNameBytes) &&
                std::find(named.begin(), named.end(), reference) == named.end())
                named.push_back(reference);
    const bool unavailableName = HasUnmatchedImageName(literalInput, matchedNameBytes);
    result.requested =
        explicitImage || (!groups.empty() && latestHasMedia && (followUp || previousVersion)) || !named.empty() || unavailableName;
    if (!result.requested)
        return result;
    if (groups.empty())
    {
        result.notice = "No image from this conversation is available to inspect. Please attach the intended image.";
        return result;
    }
    if (unavailableName)
    {
        result.notice = "That named image is not available in this conversation. Which image do you mean?";
        return result;
    }
    auto referenceInput = input;
    for (std::size_t index = 0; index < matchedNameBytes.size(); ++index)
        if (matchedNameBytes[index])
            referenceInput[index] = ' ';
    const auto referenceWords = Words(referenceInput);
    const auto originWords = OriginWords(referenceInput);
    const bool generatedVersion = GeneratedVersionReference(originWords);
    const bool uploadsOnly = NamesOrigin(originWords, false);
    const bool generatedOnly = generatedVersion || NamesOrigin(originWords, true);
    const bool priorComparison = generatedVersion && referenceWords.find(" in the comparison ") != std::string::npos;
    const bool comparison = Contains(referenceWords, {" compare ", " difference ", " differences ", " changed "}) ||
                            (!priorComparison && referenceWords.find(" comparison ") != std::string::npos);
    const bool pronounComparison =
        comparison && Contains(referenceWords, {" compare it ", " compare this ", " compare that ", " compare them ", " compare both "});
    if (uploadsOnly != generatedOnly && !pronounComparison)
    {
        const auto wanted = generatedOnly ? MediaOrigin::GeneratedArtifact : MediaOrigin::UserAttachment;
        const auto otherOrigin = [wanted](const auto& reference) { return reference.origin != wanted; };
        if (std::any_of(named.begin(), named.end(), otherOrigin))
        {
            result.notice = "That named image does not have the requested uploaded or generated origin. Which image do you mean?";
            return result;
        }
        for (auto& group : groups)
            std::erase_if(group, otherOrigin);
        std::erase_if(groups, [](const auto& group) { return group.empty(); });
        std::erase_if(focus, otherOrigin);
        std::erase_if(named, otherOrigin);
        if (groups.empty())
        {
            result.notice = generatedOnly ? "No generated image from this conversation is available."
                                          : "No uploaded image from this conversation is available.";
            return result;
        }
    }
    auto candidates = groups.back();
    if (comparison && candidates.size() == 1 && groups.size() > 1)
    {
        candidates = groups[groups.size() - 2];
        for (const auto& reference : groups.back())
            if (std::find(candidates.begin(), candidates.end(), reference) == candidates.end())
                candidates.push_back(reference);
    }
    const auto latestOrigin = [&](const MediaOrigin origin)
    {
        std::vector<conversationMediaReference> selected;
        for (auto group = groups.rbegin(); group != groups.rend(); ++group)
        {
            for (const auto& reference : *group)
                if (reference.origin == origin)
                    selected.push_back(reference);
            if (!selected.empty())
                break;
        }
        return selected;
    };
    if (comparison && uploadsOnly && generatedOnly)
    {
        candidates = latestOrigin(MediaOrigin::UserAttachment);
        const auto generated = latestOrigin(MediaOrigin::GeneratedArtifact);
        if (candidates.empty() || generated.empty())
        {
            result.notice = "Both an uploaded and a generated image are required for that comparison.";
            return result;
        }
        candidates.insert(candidates.end(), generated.begin(), generated.end());
    }
    else if (pronounComparison && uploadsOnly != generatedOnly)
    {
        if (!latestHasMedia || focus.size() != 1)
        {
            result.notice = "Which current image should be compared with that source?";
            return result;
        }
        candidates = latestOrigin(generatedOnly ? MediaOrigin::GeneratedArtifact : MediaOrigin::UserAttachment);
        if (candidates.empty())
        {
            result.notice = "That image origin is unavailable in this conversation.";
            return result;
        }
        if (std::find(candidates.begin(), candidates.end(), focus.front()) == candidates.end())
            candidates.push_back(focus.front());
    }
    if (comparison && (uploadsOnly || generatedOnly))
    {
        std::vector<conversationMediaReference> ordered;
        for (const auto& group : groups)
            for (const auto& reference : group)
                if (std::find(candidates.begin(), candidates.end(), reference) != candidates.end() &&
                    std::find(ordered.begin(), ordered.end(), reference) == ordered.end())
                    ordered.push_back(reference);
        candidates = std::move(ordered);
    }
    if (!comparison && Contains(words, {" previous image ", " previous picture ", " previous version "}) && groups.size() > 1)
        candidates = groups[groups.size() - 2];
    if (!comparison && generatedVersion && previousVersion && groups.size() > 1)
        candidates = groups[groups.size() - 2];
    const bool generatedOrdinal =
        generatedVersion && (GeneratedVersionOrdinal(originWords, 0) || GeneratedVersionOrdinal(originWords, 1) ||
                                GeneratedVersionOrdinal(originWords, 2) || GeneratedVersionOrdinal(originWords, 3));
    if (generatedOrdinal)
    {
        candidates.clear();
        for (const auto& group : groups)
            for (const auto& reference : group)
                if (reference.origin == MediaOrigin::GeneratedArtifact)
                    candidates.push_back(reference);
    }
    const std::string_view ordinals[] = {" first ", " second ", " third ", " fourth "};
    bool numbered = false;
    for (std::size_t index = 0; index < std::size(ordinals); ++index)
        if (generatedOrdinal ? GeneratedVersionOrdinal(originWords, index)
                             : referenceWords.find(ordinals[index]) != std::string::npos || NumberedImage(referenceWords, index))
        {
            numbered = true;
            if (index < candidates.size())
                result.references.push_back(candidates[index]);
            else
            {
                result.references.clear();
                result.notice = "That numbered image is not available in the referenced message. Which image do you mean?";
                return result;
            }
        }
    if (!named.empty())
    {
        if (!comparison &&
            (named.size() > 1 || (numbered && (result.references.size() != 1 || named.front() != result.references.front()))))
        {
            result.references.clear();
            result.notice = "More than one image matches those identifiers. Which image do you mean?";
            return result;
        }
        auto combined = std::move(named);
        if (pronounComparison && combined.size() == 1 && latestHasMedia && focus.size() == 1 &&
            std::find(combined.begin(), combined.end(), focus.front()) == combined.end())
            combined.push_back(focus.front());
        for (const auto& reference : result.references)
            if (std::find(combined.begin(), combined.end(), reference) == combined.end())
                combined.push_back(reference);
        result.references = std::move(combined);
        return result;
    }
    if (numbered)
    {
        if (!comparison && result.references.size() > 1)
        {
            result.references.clear();
            result.notice = "More than one numbered image was referenced. Which image do you mean?";
        }
        return result;
    }
    if (!explicitImage && !comparison && !previousVersion && focus.size() == 1)
        candidates = std::move(focus);
    if (attachmentCount || candidates.size() == 1 || comparison ||
        Contains(words, {" images ", " pictures ", " both ", " these ", " them "}))
        result.references = std::move(candidates);
    else
        result.notice = "There is more than one possible image. Which image do you mean?";
    return result;
}
}
