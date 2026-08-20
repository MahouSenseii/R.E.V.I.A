#include "Agents/conversationStylePolicy.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

namespace revia::agents
{

namespace
{
    constexpr std::string_view GenericContinuations[] = {
        "whats on your mind",
        "what is on your mind",
        "what are we working on",
        "what should we work on",
        "what would you like to work on",
        "what do you want to figure out",
        "whats the first thing you want to figure out",
        "what is the first thing you want to figure out",
        "what can i help with",
        "how can i help",
        "how can i assist you today",
        "whats next",
        "what is next",
        "is there anything else youd like to know",
        "is there anything else you would like to know",
        "if you need anything im here",
        "im here if you need anything",
        "let me know if you need anything",
        "feel free to ask",
        "if you want to check something im here",
        "if you want i can help",
        "whenever youre ready"
    };

    std::string Trim(const std::string& value)
    {
        const std::size_t first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return {};
        }
        const std::size_t last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::string NormalizeSentence(const std::string& value)
    {
        std::string normalized;
        normalized.reserve(value.size());
        bool previousWasSpace = false;
        for (const unsigned char character : value)
        {
            if (std::isalnum(character) != 0)
            {
                normalized.push_back(static_cast<char>(std::tolower(character)));
                previousWasSpace = false;
            }
            else if (std::isspace(character) != 0 && !normalized.empty() && !previousWasSpace)
            {
                normalized.push_back(' ');
                previousWasSpace = true;
            }
        }
        while (!normalized.empty() && normalized.back() == ' ')
        {
            normalized.pop_back();
        }
        return normalized;
    }

    std::string LowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    }

    std::string OpeningOf(const std::string& value)
    {
        std::size_t end = value.find_first_of(".!?\n");
        if (end == std::string::npos || end > 72)
        {
            end = std::min<std::size_t>(value.size(), 72);
        }
        else
        {
            ++end;
        }
        return Trim(value.substr(0, end));
    }
}

bool ConversationStylePolicy::LooksLikeCorrection(const std::string& input)
{
    const std::string lowered = LowerCopy(Trim(input));
    constexpr std::string_view CorrectionSignals[] = {
        "no,", "no ", "that's not", "that is not", "i'm not", "i am not",
        "wait what", "you misunderstood", "you misread", "i didn't say",
        "i did not say", "not what i", "that's wrong", "that is wrong"
    };
    return std::any_of(std::begin(CorrectionSignals), std::end(CorrectionSignals),
        [&lowered](const std::string_view signal)
        {
            return lowered.starts_with(signal) || lowered.find(signal) != std::string::npos;
        });
}

bool ConversationStylePolicy::LooksLikeBriefAcknowledgement(const std::string& input)
{
    const std::string normalized = NormalizeSentence(input);
    constexpr std::string_view Acknowledgements[] = {
        "good", "great", "nice", "okay", "ok", "cool", "fine", "not bad",
        "im good", "im okay", "doing well", "that works", "sounds good"
    };
    return std::any_of(std::begin(Acknowledgements), std::end(Acknowledgements),
        [&normalized](const std::string_view acknowledgement)
        {
            return normalized == acknowledgement;
        });
}

bool ConversationStylePolicy::LooksLikePreferenceStatement(const std::string& input)
{
    const std::string lowered = LowerCopy(Trim(input));
    constexpr std::string_view PreferenceSignals[] = {
        "i prefer ", "i like ", "i don't like ", "i do not like ",
        "my favorite ", "my favourite "
    };
    return std::any_of(std::begin(PreferenceSignals), std::end(PreferenceSignals),
        [&lowered](const std::string_view signal)
        {
            return lowered.starts_with(signal);
        });
}

bool ConversationStylePolicy::LooksLikeMotiveQuestion(const std::string& input)
{
    const std::string lowered = LowerCopy(Trim(input));
    constexpr std::string_view MotiveQuestions[] = {
        "why do you think i", "why do i ", "why would i ", "guess why i"
    };
    return std::any_of(std::begin(MotiveQuestions), std::end(MotiveQuestions),
        [&lowered](const std::string_view signal)
        {
            return lowered.find(signal) != std::string::npos;
        });
}

bool ConversationStylePolicy::LooksLikeWellbeingQuestion(const std::string& input)
{
    const std::string normalized = NormalizeSentence(input);
    constexpr std::string_view Questions[] = {
        "how are you", "how are you today", "how have you been", "are you okay",
        "are you doing okay", "how do you feel", "hows your day", "how is your day",
        "how you are"
    };
    return std::any_of(std::begin(Questions), std::end(Questions),
        [&normalized](const std::string_view question)
        {
            return normalized == question || normalized.find(question) != std::string::npos;
        });
}

bool ConversationStylePolicy::ContainsUnsupportedOperationalClaim(const std::string& reply)
{
    const std::string normalized = NormalizeSentence(reply);
    constexpr std::string_view Claims[] = {
        "alert", "pending task", "watching", "looking at", "seeing your", "checking log", "monitoring",
        "temperature", "dark mode", "light mode", "theme is", "code is compiling",
        "codes compiling", "compiler", "build is running", "process is running", "files are",
        "sitting", "standing"
    };
    return std::any_of(std::begin(Claims), std::end(Claims),
        [&normalized](const std::string_view claim)
        {
            return normalized.find(claim) != std::string::npos;
        });
}

bool ConversationStylePolicy::ContainsClaimedPreferenceAction(const std::string& reply)
{
    const std::string normalized = NormalizeSentence(reply);
    constexpr std::string_view ActionClaims[] = {
        "is on", "turned it on", "turned on", "is enabled", "enabled it",
        "mode is on", "modes on", "theme is on", "activated", "switched to",
        "changed to", "set to", "applied", "settled", "locked in", "is active",
        "still active"
    };
    return std::any_of(std::begin(ActionClaims), std::end(ActionClaims),
        [&normalized](const std::string_view claim)
        {
            return normalized.find(claim) != std::string::npos;
        });
}

bool ConversationStylePolicy::HasExplicitReason(
    const std::vector<conversationMessage>& context)
{
    bool skippedLatestUser = false;
    for (auto message = context.rbegin(); message != context.rend(); ++message)
    {
        if (message->role != "user")
        {
            continue;
        }
        if (!skippedLatestUser)
        {
            skippedLatestUser = true;
            continue;
        }

        const std::string normalized = NormalizeSentence(message->content);
        constexpr std::string_view ReasonSignals[] = {
            "because", "the reason is", "since i", "so that", "so i can"
        };
        return std::any_of(std::begin(ReasonSignals), std::end(ReasonSignals),
            [&normalized](const std::string_view signal)
            {
                return normalized.find(signal) != std::string::npos;
            });
    }
    return false;
}

bool ConversationStylePolicy::ExpressesUncertainty(const std::string& reply)
{
    const std::string normalized = NormalizeSentence(reply);
    constexpr std::string_view Signals[] = {
        "i dont know", "im not sure", "i am not sure", "you havent told",
        "you have not told", "not enough information", "i cant know", "i cannot know"
    };
    return std::any_of(std::begin(Signals), std::end(Signals),
        [&normalized](const std::string_view signal)
        {
            return normalized.find(signal) != std::string::npos;
        });
}

bool ConversationStylePolicy::SpeculatesAboutMotive(const std::string& reply)
{
    const std::string normalized = NormalizeSentence(reply);
    constexpr std::string_view Signals[] = {
        "maybe", "probably", "perhaps", "likely", "could be", "might be", "i think"
    };
    return std::any_of(std::begin(Signals), std::end(Signals),
        [&normalized](const std::string_view signal)
        {
            return normalized.find(signal) != std::string::npos;
        });
}

std::string ConversationStylePolicy::BuildTurnGuidance(
    const std::string& input,
    const std::vector<conversationMessage>& context) const
{
    std::ostringstream guidance;
    guidance << "Turn-local conversation guidance: answer the latest message as a continuation "
        "of the exchange, not as a new support ticket. Do not repeat the user's wording and do "
        "not add a generic invitation or follow-up question after the answer.";

    if (LooksLikeCorrection(input))
    {
        guidance << " The latest message appears to correct a mistaken assumption. Briefly accept "
            "the correction, replace the mistaken interpretation, and continue from the corrected "
            "fact. Do not defend, restate, or preserve the earlier assumption.";
    }
    if (LooksLikeBriefAcknowledgement(input))
    {
        guidance << " The latest message is a brief social acknowledgement. Reply with at "
            "most one short sentence. Do not add a status report, offer, or question.";
    }
    if (LooksLikePreferenceStatement(input))
    {
        guidance << " The user stated a preference, not an action request. Acknowledge the "
            "information without claiming a setting, theme, file, or application changed.";
    }
    if (LooksLikeMotiveQuestion(input))
    {
        guidance << " The user is asking about their own motive. Do not infer a reason from "
            "unrelated project context. If they have not explicitly stated why, say you do "
            "not know yet.";
    }

    std::vector<std::string> recentOpenings;
    for (auto message = context.rbegin(); message != context.rend() && recentOpenings.size() < 3;
        ++message)
    {
        if (message->role != "assistant")
        {
            continue;
        }
        const std::string opening = OpeningOf(message->content);
        if (!opening.empty() && std::find(recentOpenings.begin(), recentOpenings.end(), opening) ==
            recentOpenings.end())
        {
            recentOpenings.push_back(opening);
        }
    }
    if (!recentOpenings.empty())
    {
        guidance << " Do not reuse a complete sentence from a recent reply. Avoid these "
            "recent Revia openings:";
        for (const std::string& opening : recentOpenings)
        {
            guidance << " [" << opening << ']';
        }
    }
    return guidance.str();
}

bool ConversationStylePolicy::IsGenericContinuation(const std::string& sentence) const
{
    const std::string normalized = NormalizeSentence(sentence);
    return std::any_of(std::begin(GenericContinuations), std::end(GenericContinuations),
        [&normalized](const std::string_view candidate)
        {
            return normalized == candidate;
        });
}

std::string ConversationStylePolicy::RefineReply(
    const std::string& input,
    const std::vector<conversationMessage>& context,
    const std::string& reply) const
{
    std::string refined = Trim(reply);
    if (refined.empty())
    {
        return refined;
    }

    // These are narrow grounding gates, not canned conversation. They activate only
    // when a reply contradicts a fact we can establish from the current turn. This is
    // also the last boundary before text reaches durable context and the user.
    if (LooksLikeCorrection(input) && LooksLikeWellbeingQuestion(input))
    {
        return "Got it—you weren't saying you were down. I'm doing well.";
    }
    if (LooksLikeWellbeingQuestion(input) && ContainsUnsupportedOperationalClaim(refined))
    {
        return "I'm doing well. Curious, focused, and glad to be here with you.";
    }
    if (LooksLikePreferenceStatement(input) && ContainsClaimedPreferenceAction(refined))
    {
        return "Got it. I'll treat that as a preference, not a request to change anything.";
    }
    if (LooksLikeMotiveQuestion(input) && !HasExplicitReason(context) &&
        (!ExpressesUncertainty(refined) || SpeculatesAboutMotive(refined)))
    {
        return "I don't know yet—you've told me the preference, not the reason.";
    }
    if (LooksLikeBriefAcknowledgement(input))
    {
        if (ContainsUnsupportedOperationalClaim(refined))
        {
            return "Glad to hear it.";
        }
        const std::size_t sentenceEnd = refined.find_first_of(".!?\n");
        if (sentenceEnd != std::string::npos)
        {
            refined = Trim(refined.substr(0, sentenceEnd + 1));
        }
    }

    // Only inspect the final sentence. Removing an identical phrase from quoted or
    // explanatory material in the middle would change the answer rather than its style.
    const std::size_t searchFrom = refined.size() > 1 ? refined.size() - 2 : 0;
    const std::size_t boundary = refined.find_last_of(".!?\n", searchFrom);
    const std::size_t start = boundary == std::string::npos ? 0 : boundary + 1;
    const std::string tail = Trim(refined.substr(start));
    if (!IsGenericContinuation(tail) || start == 0)
    {
        return refined;
    }

    refined = Trim(refined.substr(0, start));
    return refined.empty() ? Trim(reply) : refined;
}

bool ConversationStylePolicy::ShouldSuppressSpokenFragment(
    const std::string& input,
    const std::vector<conversationMessage>& context,
    const std::string& fragment,
    const bool alreadySpokeFragment) const
{
    if (IsGenericContinuation(fragment))
    {
        return true;
    }
    if (LooksLikeBriefAcknowledgement(input) && alreadySpokeFragment)
    {
        return true;
    }
    if (LooksLikeWellbeingQuestion(input) && ContainsUnsupportedOperationalClaim(fragment))
    {
        return true;
    }
    if (LooksLikePreferenceStatement(input) && ContainsClaimedPreferenceAction(fragment))
    {
        return true;
    }
    return LooksLikeMotiveQuestion(input) && !HasExplicitReason(context) &&
        (!ExpressesUncertainty(fragment) || SpeculatesAboutMotive(fragment));
}

bool ConversationStylePolicy::CanStreamReply(const std::string& input) const
{
    // These turn types can require whole-reply grounding or one-sentence limiting.
    // Waiting for their short answer is preferable to speaking text that refinement
    // would immediately retract.
    return !LooksLikeWellbeingQuestion(input) &&
        !LooksLikeBriefAcknowledgement(input) &&
        !LooksLikePreferenceStatement(input) &&
        !LooksLikeMotiveQuestion(input);
}

} // namespace revia::agents
