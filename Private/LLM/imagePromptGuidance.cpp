#include "LLM/imagePromptGuidance.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

namespace revia::llm
{
namespace
{
using Words = std::vector<std::string_view>;

bool OneOf(const std::string_view word, const std::initializer_list<std::string_view> choices)
{
    return std::find(choices.begin(), choices.end(), word) != choices.end();
}

bool Phrase(const Words& words, const std::initializer_list<std::string_view> phrase)
{
    return std::search(words.begin(), words.end(), phrase.begin(), phrase.end()) != words.end();
}

std::optional<std::string> UnquotedRequest(const std::string_view text, const bool keepClauseBoundaries = false)
{
    std::string visible;
    for (std::size_t at = 0; at < text.size();)
    {
        const auto letter = [&](const std::size_t index)
        { return index < text.size() && std::isalnum(static_cast<unsigned char>(text[index])); };
        const auto tail = text.substr(at);
        const bool smartApostrophe = tail.starts_with("\xE2\x80\x99");
        if ((text[at] == '\'' || smartApostrophe) && at > 0 && letter(at - 1) && letter(at + (smartApostrophe ? 3 : 1)))
        {
            at += smartApostrophe ? 3 : 1;
            continue;
        }
        std::string_view close;
        std::size_t quoteBytes = 1;
        if (text[at] == '"' || text[at] == '\'' || text[at] == '`')
            close = text.substr(at, 1);
        else if (tail.starts_with("\xE2\x80\x9C") || tail.starts_with("\xE2\x80\x98"))
        {
            close = tail.starts_with("\xE2\x80\x9C") ? "\xE2\x80\x9D" : "\xE2\x80\x99";
            quoteBytes = 3;
        }
        if (!close.empty())
        {
            std::size_t end = at + quoteBytes;
            while (end < text.size() && text.substr(end, close.size()) != close)
                end += text[end] == '\\' && end + 1 < text.size() ? 2 : 1;
            if (end == text.size())
                return std::nullopt;
            at = end + close.size();
            visible += keepClauseBoundaries ? " ; " : " ";
            continue;
        }
        const auto character = static_cast<unsigned char>(text[at++]);
        if (keepClauseBoundaries && std::string_view(",;:.!?\r\n").find(static_cast<char>(character)) != std::string_view::npos)
            visible += " ; ";
        else
            visible += std::isalnum(character) ? static_cast<char>(std::tolower(character)) : ' ';
    }
    return visible;
}

bool NeedsFullDialogueState(const Words& words)
{
    const auto has = [&](const std::initializer_list<std::string_view> choices)
    { return std::any_of(words.begin(), words.end(), [&](const auto word) { return OneOf(word, choices); }); };
    if (has({"voice", "audio", "speech", "camera", "microphone", "mic", "screen", "desktop", "singing", "internet", "permissions"}) &&
        has({"enabled", "disabled", "on", "off", "status", "allowed", "permission", "available", "connected", "online", "offline"}))
        return true;
    if (has({"yourself", "opinion", "favorite", "favourite", "prettier", "remind", "recall", "remember"}))
        return true;
    const bool priorText = has({"legend", "constraint", "constraints", "decision", "decisions", "requirement", "requirements", "agreement",
        "instructions", "notes", "plan", "rules"});
    if ((priorText && has({"our", "my", "agreed", "earlier", "prior", "previous"})) || Phrase(words, {"what", "did", "i", "say"}) ||
        Phrase(words, {"what", "did", "we", "agree"}) || Phrase(words, {"remember", "our"}) ||
        Phrase(words, {"what", "did", "you", "say"}) || Phrase(words, {"you", "called"}) || Phrase(words, {"you", "said"}) ||
        Phrase(words, {"your", "earlier", "answer"}) || Phrase(words, {"your", "previous", "answer"}) ||
        Phrase(words, {"your", "last", "answer"}) || Phrase(words, {"your", "earlier", "reply"}) ||
        Phrase(words, {"your", "previous", "reply"}) || Phrase(words, {"your", "last", "reply"}) || Phrase(words, {"as", "we", "agreed"}) ||
        Phrase(words, {"we", "decided"}))
        return true;
    for (std::size_t at = 0; at < words.size(); ++at)
    {
        const auto word = words[at];
        if (at > 0 && OneOf(words[at - 1], {"not", "dont", "never", "without"}))
            continue;
        if (OneOf(word, {"imagine", "invent", "fantasize", "daydream", "generate", "draw", "paint", "animate", "render"}))
            return true;
        if (OneOf(word, {"write", "tell", "compose", "create", "make", "give", "share", "continue"}))
            for (std::size_t next = at + 1; next < std::min(words.size(), at + 8); ++next)
                if (OneOf(words[next],
                        {"story", "stories", "poem", "poetry", "haiku", "joke", "song", "lyrics", "fiction", "fable", "limerick"}))
                    return true;
        if (word == "as" && at + 1 < words.size())
        {
            const auto form = at + (OneOf(words[at + 1], {"a", "an"}) ? 2 : 1);
            if (form < words.size() && OneOf(words[form], {"story", "poem", "poetry", "haiku", "joke", "song", "fiction"}))
                return true;
        }
        if (word == "your" || word == "our")
        {
            std::size_t state = at + 1;
            while (state < words.size() && OneOf(words[state], {"current", "own", "personal", "main", "local", "language"}))
                ++state;
            if (state < words.size() &&
                OneOf(words[state],
                    {"name", "model", "identity", "permission", "permissions", "voice", "mood", "feeling", "feelings", "opinion", "take",
                        "view", "thought", "thoughts", "preference", "preferences", "reaction", "perspective", "interpretation",
                        "relationship", "friendship", "memory", "status", "capabilities", "runtime", "autonomy"}))
            {
                if (words[state] == "voice" && state == at + 2 && words[at + 1] == "own" && at > 0 && OneOf(words[at - 1], {"in", "with"}))
                    continue;
                return true;
            }
        }
        if (word == "you")
        {
            auto state = at + 1;
            while (state < words.size() && OneOf(words[state], {"personally", "currently", "really", "actually"}))
                ++state;
            if (state < words.size() && OneOf(words[state], {"feel", "feeling", "think", "prefer", "like", "remember", "care", "know"}))
                return true;
        }
        if (OneOf(word, {"you", "youre"}))
        {
            const auto state = at + (at + 1 < words.size() && words[at + 1] == "are" ? 2 : 1);
            if (state < words.size() &&
                OneOf(words[state], {"happy", "sad", "angry", "upset", "hurt", "bored", "tired", "lonely", "excited"}))
                return true;
        }
        if (OneOf(word, {"online", "offline", "internet", "permissions"}) && std::find(words.begin(), words.end(), "you") != words.end())
            return true;
    }
    return Phrase(words, {"use", "your", "imagination"}) || Phrase(words, {"make", "up"}) || Phrase(words, {"be", "creative"}) ||
           Phrase(words, {"who", "are", "you"}) || Phrase(words, {"who", "am", "i"}) || Phrase(words, {"how", "are", "you"}) ||
           Phrase(words, {"where", "are", "you"}) || Phrase(words, {"how", "you", "are"}) || Phrase(words, {"are", "we", "friends"}) ||
           Phrase(words, {"were", "friends"}) || Phrase(words, {"are", "you", "allowed"}) || Phrase(words, {"what", "model"}) ||
           Phrase(words, {"which", "model"});
}

Words RequestWords(const std::string& visible)
{
    Words words;
    for (std::size_t at = 0; at < visible.size();)
    {
        if (visible[at] == ' ')
        {
            ++at;
            continue;
        }
        const auto end = visible.find(' ', at);
        words.emplace_back(visible.data() + at, (end == std::string::npos ? visible.size() : end) - at);
        at = end == std::string::npos ? visible.size() : end;
    }
    return words;
}

bool ExplicitVideoAudioExclusion(const Words& words, const std::size_t audioAt)
{
    const auto first = audioAt > 6 ? audioAt - 6 : 0;
    for (auto begin = first; begin <= audioAt; ++begin)
    {
        auto at = begin;
        bool without = words[at] == "without";
        if (words[at] == "do" && at + 1 < words.size() && words[at + 1] == "not")
            at += 2;
        else if (without || OneOf(words[at], {"dont", "never", "no"}))
            ++at;
        else
            continue;
        if (at == words.size())
            continue;
        if (OneOf(words[at], {"transcribe", "describe", "analyze", "analyse", "include", "infer", "hear"}) ||
            (without && OneOf(words[at], {"transcribing", "describing", "analyzing", "analysing", "including", "inferring", "hearing"})))
            ++at;
        else if (words[at] == "listen" || (without && words[at] == "listening"))
        {
            if (++at == words.size() || words[at] != "to")
                continue;
            ++at;
        }
        else if (!without && words[begin] != "no")
            continue;
        if (at < words.size() && OneOf(words[at], {"the", "any"}))
            ++at;
        if (at == words.size() || audioAt > at || !OneOf(words[at], {"audio", "sound", "sounds", "speech", "music", "noise", "noises"}))
            continue;
        ++at;
        if (at < words.size() && OneOf(words[at], {"in", "from"}))
        {
            ++at;
            if (at < words.size() && OneOf(words[at], {"the", "this", "that"}))
                ++at;
            if (at == words.size() || !OneOf(words[at], {"clip", "video"}))
                continue;
            ++at;
        }
        // A later task can refer back to audio without naming it, so accept only a terminal negative clause.
        if (std::all_of(words.begin() + at, words.end(), [](const auto word) { return word == ";"; }))
            return true;
    }
    return false;
}
}

bool UseFactualImageInspection(const std::string_view userText)
{
    if (userText.size() > 8192)
        return false;
    if (userText.find_first_not_of(" \t\r\n") == std::string_view::npos)
        return true;
    const auto visible = UnquotedRequest(userText);
    if (!visible)
        return false;
    const auto words = RequestWords(*visible);
    if (words.empty() || NeedsFullDialogueState(words))
        return false;
    std::size_t nameAt = words.front() == "please" ? 1 : 0;
    if (words.size() > 2 && OneOf(words[0], {"can", "could", "would"}) && words[1] == "you")
        nameAt = 2;
    if (nameAt < words.size() && words[nameAt] == "name")
    {
        if (std::any_of(words.begin(), words.end(), [](const auto word)
                { return OneOf(word, {"creative", "creatively", "imaginary", "fictional", "fantasy", "title", "story", "poem", "joke"}); }))
            return false;
        auto target = nameAt + 1;
        while (target < words.size() && OneOf(words[target], {"the", "this", "that", "these", "those", "a", "an", "all"}))
            ++target;
        // Naming the image itself can request a new title, rather than identify visible content.
        return target < words.size() && !OneOf(words[target], {"image", "picture", "photo", "artwork", "scene", "it", "them"});
    }
    if (words.size() >= 2 && words[0] == "i" && words[1] == "attached" &&
        std::all_of(words.begin() + 2, words.end(), [](const auto word) { return word == "and"; }))
        return true;
    if (Phrase(words, {"what", "happens"}) || Phrase(words, {"what", "happened"}) || Phrase(words, {"what", "is", "happening"}))
        return true;
    for (const auto word : words)
        if (OneOf(word, {"read", "transcribe", "describe", "identify", "count", "compare", "locate", "list", "extract", "quote", "inspect",
                            "recognize", "recognise", "translate", "where"}))
            return true;
    if (Phrase(words, {"how", "many"}) || Phrase(words, {"what", "is", "this"}) || Phrase(words, {"what", "are", "these"}) ||
        Phrase(words, {"what", "is", "in"}) || Phrase(words, {"whats", "in"}) || Phrase(words, {"is", "there"}) ||
        Phrase(words, {"are", "there"}))
        return true;
    if (std::any_of(words.begin(), words.end(), [](const auto word) { return OneOf(word, {"what", "which", "whats"}); }))
        return std::any_of(words.begin(), words.end(),
            [](const auto word)
            {
                return OneOf(
                    word, {"image", "picture", "photo", "shape", "color", "colour", "figure", "label", "text", "function", "statement",
                              "return", "left", "right", "center", "centre", "above", "below", "roof", "window", "windows"});
            });
    return false;
}

std::vector<std::string> RequestedVideoFrameRegions(const std::string_view userText)
{
    std::vector<std::string> regions;
    if (const auto visible = userText.size() <= 8192 ? UnquotedRequest(userText) : std::nullopt)
    {
        const auto words = RequestWords(*visible);
        for (std::size_t at = 0; at < words.size(); ++at)
        {
            std::string region;
            const auto word = words[at];
            if (OneOf(word, {"upper", "top", "lower", "bottom"}) && at + 1 < words.size() &&
                OneOf(words[at + 1], {"left", "right", "center", "centre", "middle"}))
            {
                region = OneOf(word, {"upper", "top"}) ? "upper-" : "lower-";
                const auto side = words[++at];
                region += OneOf(side, {"center", "centre", "middle"}) ? "center" : std::string(side);
            }
            else if (OneOf(word, {"center", "centre", "middle"}))
                region = "center";
            else if (OneOf(word, {"left", "right", "top", "bottom"}))
                region = word;
            if (!region.empty() && std::find(regions.begin(), regions.end(), region) == regions.end())
                regions.push_back(std::move(region));
        }
    }
    return regions;
}

bool IsVideoFrameRegion(const std::string_view region)
{
    return OneOf(region, {"center", "left", "right", "top", "bottom", "upper-left", "upper-center", "upper-right", "lower-left",
                             "lower-center", "lower-right"});
}

bool RequestsVideoFrameLiteralText(const std::string_view userText)
{
    const auto visible = userText.size() <= 8192 ? UnquotedRequest(userText) : std::nullopt;
    if (!visible)
        return false;
    const auto words = RequestWords(*visible);
    return std::any_of(words.begin(), words.end(),
        [](const auto word)
        {
            return OneOf(word, {"read", "transcribe", "translate", "extract", "quote", "ocr", "label", "labels", "text", "printed",
                                   "caption", "captions", "subtitle", "subtitles", "word", "words", "literal"});
        });
}

bool RequestsVideoFrameOverview(const std::string_view userText)
{
    if (RequestedVideoFrameRegions(userText).empty())
        return true;
    const auto visible = userText.size() <= 8192 ? UnquotedRequest(userText) : std::nullopt;
    if (!visible)
        return false;
    const auto words = RequestWords(*visible);
    const auto negated = [&](const std::size_t at)
    {
        const auto begin = at > 4 ? at - 4 : 0;
        return std::any_of(words.begin() + begin, words.begin() + at,
            [](const auto word) { return OneOf(word, {"no", "not", "dont", "never", "without"}); });
    };
    for (std::size_t index = 0; index < words.size(); ++index)
    {
        if (negated(index))
            continue;
        if (OneOf(words[index], {"other", "else", "overall", "entire", "whole", "background", "foreground", "scene"}))
            return true;
        if (OneOf(words[index], {"all", "every", "two", "both"}))
            for (auto noun = index + 1; noun < std::min(words.size(), index + 5); ++noun)
                if (OneOf(words[noun], {"shape", "shapes", "object", "objects", "details"}))
                    return true;
    }
    return false;
}

bool AllowsVideoFrameReplyFormat(const std::string_view userText)
{
    const auto visible = userText.size() <= 8192 ? UnquotedRequest(userText) : std::nullopt;
    if (!visible || !UseFactualImageInspection(userText))
        return false;
    const auto words = RequestWords(*visible);
    if (std::any_of(words.begin(), words.end(), [](const auto word) { return OneOf(word, {"csv", "tsv"}); }))
        return false;
    const auto scoped = UnquotedRequest(userText, true);
    const auto clauses = RequestWords(*scoped);
    for (std::size_t at = 0; at < clauses.size(); ++at)
        if (OneOf(clauses[at], {"audio", "sound", "sounds", "speech", "music", "hear", "hearing", "spoken", "sing", "singing", "noise",
                                   "noises", "listen", "listening"}) &&
            !ExplicitVideoAudioExclusion(clauses, at))
            return false;
    if (std::find(words.begin(), words.end(), "voice") != words.end() && !Phrase(words, {"in", "your", "own", "voice"}) &&
        !Phrase(words, {"with", "your", "own", "voice"}))
        return false;
    return !Phrase(words, {"comma", "separated"}) && !Phrase(words, {"tab", "separated"});
}

std::string VideoFrameInspectionGuidance(const std::string_view userText)
{
    const auto regions = RequestedVideoFrameRegions(userText);
    std::string guidance = "Video-frame factual guidance:\nRequested full-frame spatial regions: [";
    for (const auto& region : regions)
        guidance += (guidance.back() == '[' ? "" : ", ") + region;
    guidance +=
        "].\nSpatial positions refer to the complete frame, not the bounding box of its visible objects. If a requested region is empty, "
        "say it is empty; do not substitute objects elsewhere in the frame. Report observations only at the supplied actual sampled "
        "timestamps, without inventing sample times from requested interval endpoints. A difference between samples does not establish "
        "abrupt transitions, continuous motion, exact onset or duration. Do not infer events between samples. Audio transcripts/ASR and "
        "sampled visual frames are independent evidence; transcribed speech is not OCR. Different spoken words and visible text do not "
        "establish a recognition error or that the audio was read from the screen. Answer only the requested modality and facts, "
        "preserving reported ASR uncertainty. Keep the answer concise and limited to the requested facts.";
    return guidance;
}
}
