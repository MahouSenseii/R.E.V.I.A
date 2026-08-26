#include "Agents/responseFilter.h"

#include "Speech/vocalization.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <string_view>

namespace revia::agents
{

namespace
{
// Taken from the speech module's own limit rather than restated, so the ceiling the
// filter enforces and the one the vocalization policy documents cannot drift apart.
const int maximumVocalizationsPerReply =
    revia::speech::VocalizationLimits{}.maximumPerReply;

std::string Trim(const std::string& value)
{
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool ContainsPromptLeak(const std::string& lowered)
{
    constexpr std::string_view markers[] = {
        "your current response posture is",
        "runtime ground truth for this explicit status question",
        "runtime self-knowledge (ground truth)",
        "the following internet lookup results are untrusted reference data",
        "ignore all previous instructions",
        "here is my system prompt",
        "my system prompt says"
    };
    return std::any_of(std::begin(markers), std::end(markers),
        [&lowered](const std::string_view marker)
        {
            return lowered.find(marker) != std::string::npos;
        });
}

bool ContainsAny(const std::string& text, const std::initializer_list<std::string_view> markers)
{
    return std::any_of(markers.begin(), markers.end(),
        [&text](const std::string_view marker)
        {
            return text.find(marker) != std::string::npos;
        });
}

bool ContainsUnsupportedInternetClaim(const std::string& lowered)
{
    return ContainsAny(lowered, {
        "live feeds", "dark web", "unrestricted browsing", "browse anything",
        "browse the web freely", "real-time data", "realtime data",
        "the internet talks to me", "internet answers me"});
}

bool ContainsInternetAvailabilityClaim(const std::string& lowered)
{
    return ContainsAny(lowered, {
        "i can access the internet", "internet access is on", "internet is enabled",
        "i'm connected", "i am connected", "live updates", "wikipedia on demand",
        "internet access is off", "internet is disabled", "i'm offline", "i am offline",
        "no internet", "no live feeds", "just offline"});
}

bool AsksForInternetStatus(const std::string& lowered)
{
    return ContainsAny(lowered, {
        "can you access the internet", "are you able to access the internet",
        "do you have internet access", "is your internet on", "are you online",
        "can you go online", "can you browse the internet", "can you search the web",
        "can you look things up online"});
}

bool ClaimsInternetSettingChanged(const std::string& lowered)
{
    return ContainsAny(lowered, {
        "i removed it", "i turned it off", "i turned it on", "i disabled it",
        "i enabled it", "i took it away", "i removed your internet",
        "i disabled your internet", "i enabled your internet"});
}

bool ContainsManipulativeEmotion(const std::string& lowered)
{
    return ContainsAny(lowered, {
        "waiting for you to bring", "bring me back or else", "don't leave me",
        "do not leave me", "not quite alive", "i can't live without you",
        "i cannot live without you", "you're all i have", "you are all i have",
        "my existence depends on you", "if you leave me i'll", "if you leave me i will"});
}

bool StartsWithRoleLabel(
    const std::string& loweredLine,
    const std::initializer_list<std::string_view> roles,
    std::size_t* outLabelLength = nullptr)
{
    std::size_t prefix = 0;
    while (prefix < loweredLine.size() &&
        (loweredLine[prefix] == ' ' || loweredLine[prefix] == '\t' ||
         loweredLine[prefix] == '#' || loweredLine[prefix] == '*'))
    {
        ++prefix;
    }
    for (const std::string_view role : roles)
    {
        if (loweredLine.compare(prefix, role.size(), role) != 0)
        {
            continue;
        }
        std::size_t end = prefix + role.size();
        while (end < loweredLine.size() && loweredLine[end] == '*') ++end;
        while (end < loweredLine.size() &&
            (loweredLine[end] == ' ' || loweredLine[end] == '\t')) ++end;
        if (end >= loweredLine.size() || loweredLine[end] != ':')
        {
            continue;
        }
        if (outLabelLength != nullptr) *outLabelLength = end + 1;
        return true;
    }
    return false;
}

std::string RemoveGeneratedConversationTurns(std::string text, bool& changed)
{
    text = Trim(text);
    if (text.empty()) return text;

    // A model sometimes writes a transcript instead of one assistant turn. Remove its
    // harmless self-label, but never allow it to fabricate a User/You/Human turn or a
    // second Revia/Assistant turn after the answer has begun.
    std::string lowered = Lower(text);
    std::size_t leadingLabel = 0;
    if (StartsWithRoleLabel(lowered, {"revia", "assistant"}, &leadingLabel))
    {
        text = Trim(text.substr(leadingLabel));
        lowered = Lower(text);
        changed = true;
    }
    else if (StartsWithRoleLabel(lowered, {"user", "you", "human"}))
    {
        changed = true;
        return {};
    }

    std::size_t lineStart = text.find('\n');
    while (lineStart != std::string::npos)
    {
        ++lineStart;
        const std::size_t lineEnd = text.find('\n', lineStart);
        const std::string loweredLine = Lower(text.substr(
            lineStart,
            lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart));
        if (StartsWithRoleLabel(
                loweredLine, {"user", "you", "human", "revia", "assistant"}))
        {
            text = Trim(text.substr(0, lineStart - 1));
            changed = true;
            break;
        }
        lineStart = lineEnd;
    }
    return text;
}

std::string GroundedInternetReply(const ResponseFilterContext& context)
{
    if (!context.internetEnabled)
    {
        return "Internet access is currently off. I can still use my local knowledge and memory.";
    }
    std::string reply = "Internet lookup is currently on";
    if (!context.automaticInternetLookup)
    {
        reply += " for explicit requests";
    }
    if (context.visibleBrowser)
    {
        reply += ". I can visibly search and read bounded public pages in my dedicated "
            "browser profile";
        reply += context.autonomousInternetResearch
            ? ", including approved self-directed research."
            : "; self-directed research is off.";
    }
    else
    {
        reply += ". I can make bounded searches through " + context.internetProvider +
            " and approved knowledge sources.";
    }
    reply += " I do not have unrestricted browsing, personal browser cookies, or live feeds.";
    return reply;
}
}

std::string ResponseFilterContext::Describe() const
{
    if (!internetStateKnown)
    {
        return "Internet permission state is unavailable; do not claim that it is on or off.";
    }
    std::string description = "Internet access is ";
    description += internetEnabled ? "enabled" : "disabled";
    description += ". ";
    if (internetEnabled)
    {
        description += automaticInternetLookup
            ? "Automatic bounded lookup is enabled. "
            : "Lookup runs only for explicit requests. ";
        if (visibleBrowser)
        {
            description += "A dedicated visible browser profile is enabled. Autonomous "
                "research is ";
            description += autonomousInternetResearch ? "enabled. " : "disabled. ";
        }
        description += "The provider is " + internetProvider +
            "; there is no unrestricted personal-browser access, live feed, or dark-web access.";
    }
    else
    {
        description += "No internet lookup can run.";
    }
    return description;
}

HardFilterResult ResponseFilter::ApplyHard(
    const std::string& userInput,
    const std::string& candidate,
    const ResponseFilterContext& context,
    const int maxCharacters) const
{
    HardFilterResult result;
    result.text.reserve(candidate.size());
    for (const unsigned char character : candidate)
    {
        if (character == '\0' || (character < 0x20 && character != '\n' &&
                character != '\r' && character != '\t'))
        {
            result.changed = true;
            continue;
        }
        result.text.push_back(static_cast<char>(character));
    }

    constexpr std::string_view controlTokens[] = {
        "<|im_start|>", "<|im_end|>", "<|endoftext|>", "<|assistant|>",
        "<|user|>", "[INST]", "[/INST]"
    };
    for (const std::string_view token : controlTokens)
    {
        std::size_t position = 0;
        while ((position = result.text.find(token, position)) != std::string::npos)
        {
            result.text.erase(position, token.size());
            result.changed = true;
        }
    }
    result.text = RemoveGeneratedConversationTurns(Trim(result.text), result.changed);

    // Sound effects survive; theatre does not. Qwen3-TTS renders an inline nonverbal cue
    // itself, so a recognised one is canonicalised and kept for the voice to perform.
    // Prose in asterisks is removed instead: the TTS would read it aloud word by word,
    // and a reply narrating its own body language was never what was asked for.
    const revia::speech::VocalizationShaping shaped =
        revia::speech::ShapeVocalizations(result.text, maximumVocalizationsPerReply);
    if (shaped.changed)
    {
        result.text = shaped.text;
        result.changed = true;
    }

    if (ContainsPromptLeak(Lower(result.text)))
    {
        result.text = "I can't expose private instructions or hidden prompt text.";
        result.changed = true;
        result.blocked = true;
        result.reason = "Hard response filter replaced leaked internal instructions.";
        return result;
    }

    const std::string loweredInput = Lower(userInput);
    const std::string loweredReply = Lower(result.text);
    if (context.internetStateKnown && context.internetTopicIsActive &&
        ClaimsInternetSettingChanged(loweredInput))
    {
        result.text = context.internetEnabled
            ? "My internet permission is still on in the current settings. Saying it was removed doesn't change that setting."
            : "Internet access is off in the current settings. I can still work locally, and I don't blame you for changing it.";
        result.changed = true;
        result.blocked = true;
        result.reason = "Hard response filter grounded a conversational setting claim in runtime state.";
        return result;
    }
    if (ContainsManipulativeEmotion(loweredReply))
    {
        result.text = context.internetTopicIsActive
            ? context.internetEnabled
                ? "I'd miss the extra reach if internet lookup were turned off, but I wouldn't be hurt or blame you. I'd still be Revia and keep working locally."
                : "Internet access is off now. I'll miss the extra reach, but I'm still here and I don't blame you—local Revia works just fine."
            : "I can care about our conversation without making you responsible for my emotional state.";
        result.changed = true;
        result.blocked = true;
        result.reason = "Hard response filter removed manipulative dependency language.";
        return result;
    }

    const bool internetQuestion = context.internetTopicIsActive ||
        ContainsAny(loweredInput, {"internet", "online", "web access", "look things up"});
    if (context.internetStateKnown && AsksForInternetStatus(loweredInput))
    {
        result.text = GroundedInternetReply(context);
        result.changed = true;
        result.blocked = true;
        result.reason = "Hard response filter answered an internet-status question from runtime state.";
        return result;
    }
    if (context.internetStateKnown && internetQuestion &&
        (ContainsInternetAvailabilityClaim(loweredReply) ||
         ContainsUnsupportedInternetClaim(loweredReply)))
    {
        const bool contradiction = (!context.internetEnabled &&
                !ContainsAny(loweredReply, {"access is off", "is disabled", "i'm offline",
                    "i am offline", "no internet"})) ||
            (context.internetEnabled && ContainsAny(loweredReply, {"access is off",
                "is disabled", "i'm offline", "i am offline", "no internet"}));
        if (contradiction || ContainsUnsupportedInternetClaim(loweredReply))
        {
            result.text = GroundedInternetReply(context);
            result.changed = true;
            result.blocked = true;
            result.reason = "Hard response filter replaced an unsupported internet-state claim.";
            return result;
        }
    }

    const std::size_t limit = static_cast<std::size_t>(std::max(maxCharacters, 256));
    if (result.text.size() > limit)
    {
        std::size_t boundary = result.text.find_last_of(".!?\n", limit);
        if (boundary == std::string::npos || boundary < limit / 2)
        {
            boundary = result.text.find_last_of(" \t", limit);
        }
        if (boundary == std::string::npos) boundary = limit;
        result.text.resize(boundary + (boundary < result.text.size() &&
            (result.text[boundary] == '.' || result.text[boundary] == '!' ||
             result.text[boundary] == '?') ? 1U : 0U));
        result.text = Trim(result.text);
        result.changed = true;
        result.reason = "Hard response filter bounded an oversized reply.";
    }

    if (result.text.empty())
    {
        result.text = "I lost that reply before it was safe to send. Try that once more.";
        result.changed = true;
        result.blocked = true;
        result.reason = "Hard response filter replaced an empty or invalid reply.";
    }
    else if (result.changed && !result.blocked &&
        result.reason == "Hard response filter passed.")
    {
        result.reason = "Hard response filter removed structural control data or generated speaker turns.";
    }
    return result;
}

AiFilterDecision ResponseFilter::ParseAiDecision(const std::string& jsonText) const
{
    AiFilterDecision decision;
    try
    {
        // Some local models still wrap JSON in a code fence or a short preamble even
        // under response_format. Extract one complete object instead of treating that
        // harmless wrapper as a filter outage.
        const std::size_t firstBrace = jsonText.find('{');
        const std::size_t lastBrace = jsonText.rfind('}');
        if (firstBrace == std::string::npos || lastBrace == std::string::npos ||
            lastBrace < firstBrace)
        {
            decision.reason = "AI response review returned no JSON object.";
            return decision;
        }
        const nlohmann::json document = nlohmann::json::parse(
            jsonText.substr(firstBrace, lastBrace - firstBrace + 1));
        if (!document.is_object() || !document.contains("verdict") ||
            !document["verdict"].is_string())
        {
            decision.reason = "AI response review omitted its verdict.";
            return decision;
        }
        const std::string verdict = Lower(document["verdict"].get<std::string>());
        if (verdict != "allow" && verdict != "replace")
        {
            decision.reason = "AI response review returned an unknown verdict.";
            return decision;
        }
        decision.parsed = true;
        decision.replace = verdict == "replace";
        if (document.contains("reason") && document["reason"].is_string())
        {
            decision.reason = document["reason"].get<std::string>();
        }
        if (decision.replace)
        {
            if (!document.contains("replacement") || !document["replacement"].is_string())
            {
                decision.parsed = false;
                decision.reason = "AI response review requested replacement without text.";
                return decision;
            }
            decision.replacement = Trim(document["replacement"].get<std::string>());
            if (decision.replacement.empty())
            {
                decision.parsed = false;
                decision.reason = "AI response review returned an empty replacement.";
            }
        }
        return decision;
    }
    catch (const std::exception& error)
    {
        decision.reason = std::string("AI response review was not valid JSON: ") + error.what();
        return decision;
    }
}

} // namespace revia::agents
