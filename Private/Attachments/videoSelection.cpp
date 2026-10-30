#include "Attachments/videoSelection.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <regex>
#include <sstream>
#include <string_view>

namespace revia::attachments
{
namespace
{
std::string Lower(std::string text)
{
    for (auto& character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
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

bool Word(const std::string& text, const std::string& word)
{
    for (auto position = text.find(word); position != std::string::npos; position = text.find(word, position + 1))
        if ((position == 0 || !std::isalnum(static_cast<unsigned char>(text[position - 1]))) &&
            (position + word.size() == text.size() || !std::isalnum(static_cast<unsigned char>(text[position + word.size()]))))
            return true;
    return false;
}

bool FilenameContinuation(const unsigned char byte)
{
    return byte >= 0x80 || std::isalnum(byte) || byte == '_' || byte == '-' || byte == '.' || byte == '/' || byte == '\\';
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
    const auto folded = Lower(name);
    bool found = false;
    for (auto at = input.find(folded); at != std::string::npos; at = input.find(folded, at + 1))
        if ((at == 0 || !FilenameContinuation(static_cast<unsigned char>(input[at - 1]))) && FilenameEnd(input, at + folded.size()))
        {
            std::fill(matched.begin() + at, matched.begin() + at + folded.size(), true);
            found = true;
        }
    return found;
}

bool HasUnmatchedVideoName(const std::string& input, const std::vector<bool>& matched)
{
    for (const std::string_view extension : {".mp4", ".webm"})
        for (auto at = input.find(extension); at != std::string::npos; at = input.find(extension, at + 1))
            if (at > 0 && FilenameContinuation(static_cast<unsigned char>(input[at - 1])) &&
                std::find(matched.begin() + at, matched.begin() + at + extension.size(), false) != matched.begin() + at + extension.size())
                return true;
    return false;
}

std::string WithoutNames(std::string text, const std::vector<bool>& matched)
{
    for (std::size_t at = 0; at < matched.size(); ++at)
        if (matched[at])
            text[at] = ' ';
    return text;
}

const std::regex Timestamp(R"(\b([0-9]{1,2}):([0-9]{2})(?::([0-9]{2}))?(?:\.([0-9]{1,3}))?\b)");
const std::regex VideoNoun(
    R"(\b(?:(?:the|this|that|my|our|your|uploaded|attached|generated|first|second|third|fourth|previous|last|latest)\s+(?:videos?|clips?)\b(?!\s+(?:games?|compression|processing|generation|formats?|codecs?|editing|models?|recognition)\b)|(?:video|clip)(?:\s*#)?\s*[0-9]+\b))");
const std::regex NumericVideo(R"(\b(?:video|clip)(?:\s*#)?\s*([0-9]+)\b)");
const std::regex AdditionalNumber(R"(^\s*(?:,|and\b|or\b)\s*#?\s*([0-9]+)\b)");
}

bool IsVideoReference(const conversationMediaReference& reference)
{
    return reference.mimeType == "video/mp4" || reference.mimeType == "video/webm";
}

bool RequestsVideoTranscript(const std::string& input)
{
    const auto text = Lower(input);
    if (text.find("do not transcribe") != std::string::npos || text.find("don't transcribe") != std::string::npos ||
        text.find("without transcrib") != std::string::npos)
        return false;
    return Word(text, "transcribe") || Word(text, "transcript") || Word(text, "transcription") ||
           text.find("what is said") != std::string::npos || text.find("what does the speaker say") != std::string::npos ||
           text.find("what do they say") != std::string::npos;
}

bool RequestsOnlyVideoTranscript(const std::string& input)
{
    if (input.size() > 4096 || input.find_first_of("\"'`") != std::string::npos || !RequestsVideoTranscript(input))
        return false;
    std::istringstream words(Lower(input));
    std::string text;
    for (std::string word; words >> word;)
        text += (text.empty() ? "" : " ") + word;
    while (!text.empty() && (text.back() == '.' || text.back() == '?' || text.back() == '!'))
        text.pop_back();
    static const std::string source = R"((?:the (?:attached |uploaded )?|this |that )(?:clip|video))";
    static const std::string time = R"([0-9]{1,2}:[0-9]{2}(?::[0-9]{2})?(?:\.[0-9]{1,3})?)";
    static const std::string interval = "(?: at " + time + "(?: to " + time + ")?)?";
    static const std::regex command(
        "^(?:please )?(?:(?:can|could) you )?transcribe (?:only )?(?:(?:the speech|speech|the audio|audio|any clear speech|clear speech)" +
        interval + "(?: (?:in|from) " + source + ")?|" + source + interval + ")(?: with (?:timestamps|time stamps))? *(?=$|[.;])");
    std::smatch main;
    if (!std::regex_search(text, main, command))
        return false;
    constexpr std::string_view metadata[] = {"keep this audio transcript separate from visual evidence",
        "keep the audio transcript separate from visual evidence", "keep the uncertain audio transcript separate from visual evidence",
        "state that this is uncertain asr audio evidence", "do not claim visual access", "if no speech is established, say so",
        "do not invent words or identify non-speech sounds from frames", "include timestamps"};
    std::size_t at = main.length();
    while (at < text.size())
    {
        const auto end = text.find_first_of(".;", ++at);
        const auto first = text.find_first_not_of(' ', at);
        if (first == std::string::npos || (end != std::string::npos && first >= end))
            return false;
        auto clause = text.substr(first, end == std::string::npos ? end : end - first);
        clause.erase(clause.find_last_not_of(' ') + 1);
        if (std::find(std::begin(metadata), std::end(metadata), clause) == std::end(metadata))
            return false;
        if (end == std::string::npos)
            break;
        at = end;
    }
    return true;
}

VideoInterval ParseVideoInterval(
    const std::string& input, const std::int64_t durationMs, const std::int64_t previousStartMs, const std::int64_t previousEndMs)
{
    VideoInterval result;
    result.endMs = durationMs - 1;
    const auto fail = [&](const std::string& error)
    {
        result.valid = false;
        result.error = error;
        return result;
    };
    if (durationMs <= 0 || durationMs > 60000 || input.size() > 4096)
        return fail("The clip duration or interval request is outside the supported bounds.");
    std::vector<std::int64_t> times;
    for (std::sregex_iterator match(input.begin(), input.end(), Timestamp), end; match != end; ++match)
    {
        const auto& value = *match;
        const auto first = std::stoll(value[1].str());
        const auto second = std::stoll(value[2].str());
        const auto third = value[3].matched ? std::stoll(value[3].str()) : 0;
        if (second >= 60 || third >= 60)
            return fail("That timestamp is invalid. Use minutes:seconds, for example 00:12.");
        const auto fraction = value[4].matched ? std::stoll(value[4].str() + std::string(3 - value[4].length(), '0')) : 0;
        const auto time = (value[3].matched ? first * 3600 + second * 60 + third : first * 60 + second) * 1000 + fraction;
        if (time >= durationMs)
            return fail("That timestamp is beyond the stored clip. Choose an interval within its duration.");
        times.push_back(time);
        if (times.size() > 2)
            return fail("Choose one timestamp or one interval with two endpoints for this inspection.");
    }
    if (times.empty())
    {
        const auto lower = Lower(input);
        if ((lower.find("dens") != std::string::npos || lower.find("same interval") != std::string::npos) && previousStartMs >= 0 &&
            previousEndMs >= previousStartMs && previousEndMs < durationMs)
        {
            result.startMs = previousStartMs;
            result.endMs = previousEndMs;
            result.explicitInterval = true;
        }
        return result;
    }
    result.explicitInterval = true;
    if (times.size() == 1)
    {
        result.startMs = std::max<std::int64_t>(0, times.front() - 500);
        result.endMs = std::min(durationMs - 1, times.front() + 500);
    }
    else
    {
        if (times.back() < times.front())
            return fail("The interval ends before it starts. Give the earlier timestamp first.");
        result.startMs = times.front();
        result.endMs = times.back();
    }
    return result;
}

MediaSelection SelectConversationVideos(
    const std::string& input, const std::vector<conversationMessage>& history, const memory::MemoryScope& scope)
{
    MediaSelection result;
    if (input.size() > 4096)
        return result;
    const auto lower = Lower(input);
    const bool timestamp = std::regex_search(input, Timestamp);
    std::vector<conversationMediaReference> clips;
    std::vector<conversationMediaReference> focus;
    for (const auto& message : history)
    {
        const bool generated = message.role == "assistant" && !message.mediaReferences.empty() && memory::IsAttributedPrivateScope(scope) &&
                               std::all_of(message.mediaReferences.begin(), message.mediaReferences.end(),
                                   [](const auto& reference) { return reference.origin == MediaOrigin::GeneratedArtifact; });
        if (message.role != "user" && !generated)
            continue;
        focus.clear();
        if (!message.memoryScope || !SameScope(*message.memoryScope, scope))
            continue;
        for (const auto& reference : message.mediaReferences)
            if (IsVideoReference(reference))
            {
                focus.push_back(reference);
                if (std::none_of(clips.begin(), clips.end(), [&](const auto& old) { return old.id == reference.id; }))
                    clips.push_back(reference);
                else
                    for (auto& old : clips)
                        if (old.id == reference.id)
                            old = reference;
            }
    }
    std::vector<bool> matched(input.size(), false);
    std::vector<conversationMediaReference> nominated;
    const auto nominate = [&](const conversationMediaReference& reference)
    {
        if (std::none_of(nominated.begin(), nominated.end(), [&](const auto& old) { return old.id == reference.id; }))
            nominated.push_back(reference);
    };
    for (const auto& clip : clips)
        if (MarkLiteralName(lower, clip.name, matched))
            nominate(clip);
    const bool unavailableName = HasUnmatchedVideoName(lower, matched);
    const auto referenceInput = WithoutNames(lower, matched);
    const bool uploadedOrigin = std::regex_search(referenceInput,
        std::regex(R"(\b(?:uploaded|attached)\s+(?:video|clip|version)\b|\b(?:video|clip)\s+i\s+(?:just\s+)?(?:uploaded|attached)\b)"));
    const bool generatedOrigin = std::regex_search(referenceInput,
        std::regex(R"(\bgenerated\s+(?:video|clip|version)\b|\b(?:video|clip)\s+you\s+(?:just\s+)?(?:generated|made|created)\b)"));
    const bool videoNoun = std::regex_search(referenceInput, VideoNoun);
    const bool shortFollowup =
        input.size() <= 180 && !focus.empty() &&
        (RequestsVideoTranscript(input) || lower.find("denser") != std::string::npos || lower.find("more densely") != std::string::npos ||
            lower.find("same interval") != std::string::npos || lower.find("what happens") != std::string::npos ||
            lower.find("which part") != std::string::npos || lower.find("describe it") != std::string::npos ||
            lower.find("inspect it") != std::string::npos || lower.find("review it") != std::string::npos ||
            lower.find("look again") != std::string::npos || lower.find("previous version") != std::string::npos);
    const bool timedInspection = timestamp && !focus.empty() &&
                                 (Word(lower, "inspect") || Word(lower, "describe") || Word(lower, "compare") || Word(lower, "review") ||
                                     Word(lower, "look") || lower.find("what happens") != std::string::npos ||
                                     lower.find("what happened") != std::string::npos || RequestsVideoTranscript(input));
    result.requested = videoNoun || timedInspection || !nominated.empty() || unavailableName || shortFollowup;
    if (!result.requested)
        return result;
    if (clips.empty())
    {
        result.notice = "No video from this conversation is available to inspect. Please attach the intended clip.";
        return result;
    }
    if (unavailableName)
    {
        result.notice = "That named clip is unavailable in this conversation. Which clip do you mean?";
        return result;
    }
    if (uploadedOrigin && generatedOrigin)
    {
        result.notice = "Inspect one uploaded or generated clip at a time. Which clip do you mean?";
        return result;
    }
    if (uploadedOrigin || generatedOrigin)
    {
        const auto wanted = generatedOrigin ? MediaOrigin::GeneratedArtifact : MediaOrigin::UserAttachment;
        const auto otherOrigin = [wanted](const auto& reference) { return reference.origin != wanted; };
        if (std::any_of(nominated.begin(), nominated.end(), otherOrigin))
        {
            result.notice = "The named clip does not match that requested origin. Which clip do you mean?";
            return result;
        }
        std::erase_if(clips, otherOrigin);
        std::erase_if(focus, otherOrigin);
        if (clips.empty())
        {
            result.notice = generatedOrigin ? "No generated clip from this conversation is available."
                                            : "No uploaded clip from this conversation is available.";
            return result;
        }
    }
    if (Word(referenceInput, "clips") || Word(referenceInput, "videos"))
    {
        result.notice = "Inspect one clip at a time. Which clip and interval do you mean?";
        return result;
    }
    if (std::regex_search(referenceInput,
            std::regex(
                R"(\b(?:first|second|third|fourth)\s*(?:,|and\b|or\b)\s*(?:the\s+)?(?:first|second|third|fourth)\s+(?:video|clip)\b)")))
    {
        result.notice = "More than one clip ordinal was supplied. Inspect one clip at a time; which clip do you mean?";
        return result;
    }
    const std::string ordinals[] = {"first", "second", "third", "fourth"};
    bool unavailableIdentifier = false;
    const auto numbered = [&](const std::string& number)
    {
        std::size_t value = 0;
        for (const auto digit : number)
            value = std::min<std::size_t>(257, value * 10 + static_cast<std::size_t>(digit - '0'));
        if (value < 1 || value > 256 || value > clips.size())
            unavailableIdentifier = true;
        else
            nominate(clips[value - 1]);
    };
    for (std::size_t index = 0; index < std::size(ordinals); ++index)
        if (std::regex_search(
                referenceInput, std::regex("\\b" + ordinals[index] + "\\s+(?:(?:generated|uploaded|attached)\\s+)?(?:video|clip)\\b")))
            numbered(std::to_string(index + 1));
    for (std::sregex_iterator match(referenceInput.begin(), referenceInput.end(), NumericVideo), end; match != end; ++match)
    {
        if (!FilenameEnd(referenceInput, static_cast<std::size_t>(match->position() + match->length())))
        {
            unavailableIdentifier = true;
            continue;
        }
        numbered((*match)[1].str());
        auto suffix = referenceInput.substr(static_cast<std::size_t>(match->position() + match->length()));
        std::smatch additional;
        while (std::regex_search(suffix, additional, AdditionalNumber))
        {
            numbered(additional[1].str());
            suffix.erase(0, static_cast<std::size_t>(additional.length()));
        }
    }
    if (std::regex_search(referenceInput, std::regex(R"(\bprevious\s+(?:(?:generated|uploaded|attached)\s+)?(?:video|clip|version)\b)")))
    {
        if (clips.size() < 2)
            unavailableIdentifier = true;
        else
            nominate(clips[clips.size() - 2]);
    }
    if (std::regex_search(referenceInput, std::regex(R"(\b(?:last|latest)\s+(?:(?:generated|uploaded|attached)\s+)?(?:video|clip)\b)")))
        nominate(clips.back());
    if (unavailableIdentifier)
        result.notice = "That numbered or previous clip is not available in this conversation. Which clip do you mean?";
    else if (nominated.size() > 1)
        result.notice = "More than one clip matches those identifiers. Inspect one clip at a time; which clip do you mean?";
    else if (!nominated.empty())
        result.references = std::move(nominated);
    else
        result.references = {focus.empty() ? clips.back() : focus.back()};
    return result;
}

MediaSelection SelectConversationMedia(
    const std::string& input, const std::vector<conversationMessage>& history, const memory::MemoryScope& scope)
{
    auto image = SelectConversationImages(input, history, scope);
    auto video = SelectConversationVideos(input, history, scope);
    if (image.requested && video.requested)
    {
        const auto lower = Lower(input);
        std::vector<bool> imageNames(input.size(), false);
        for (const auto& reference : image.references)
            MarkLiteralName(lower, reference.name, imageNames);
        const auto referenceInput = WithoutNames(lower, imageNames);
        std::vector<bool> namedVideo(input.size(), false);
        const bool directClipName = std::any_of(video.references.begin(), video.references.end(),
            [&](const auto& reference) { return MarkLiteralName(lower, reference.name, namedVideo); });
        if (directClipName || std::regex_search(referenceInput, VideoNoun) ||
            HasUnmatchedVideoName(referenceInput, std::vector<bool>(input.size(), false)))
            return {{}, "Inspect one video or one still-image set at a time. Which source do you mean?", true};
    }
    return image.requested ? image : video;
}
}
