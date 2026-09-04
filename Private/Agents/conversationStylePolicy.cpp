#include "Agents/conversationStylePolicy.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <utility>

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
        "whenever youre ready",
        "you okay with that",
        "are you okay with that",
        "should i ask you to take it away again",
        "or should i ask you to take it away again",
        "im not complaining",
        "i am not complaining",
        "just curious"
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

    std::string LimitSentences(const std::string& value, const int maximum)
    {
        int sentences = 0;
        for (std::size_t index = 0; index < value.size(); ++index)
        {
            if (value[index] != '.' && value[index] != '!' && value[index] != '?')
            {
                continue;
            }
            while (index + 1 < value.size() &&
                (value[index + 1] == '.' || value[index + 1] == '!' ||
                 value[index + 1] == '?'))
            {
                ++index;
            }
            ++sentences;
            if (sentences >= maximum)
            {
                return Trim(value.substr(0, index + 1));
            }
        }
        return Trim(value);
    }

    std::string RemoveTrailingQuestion(const std::string& value)
    {
        const std::string trimmed = Trim(value);
        if (trimmed.empty() || trimmed.back() != '?') return trimmed;
        const std::size_t previous = trimmed.find_last_of(".!?", trimmed.size() - 2);
        return previous == std::string::npos
            ? trimmed
            : Trim(trimmed.substr(0, previous + 1));
    }

    std::size_t TrailingSentenceStart(const std::string& value)
    {
        if (value.empty()) return 0;

        std::size_t scan = value.size();
        while (scan > 0 &&
            (value[scan - 1] == '.' || value[scan - 1] == '!' || value[scan - 1] == '?'))
        {
            --scan;
        }
        while (scan > 0)
        {
            const char character = value[scan - 1];
            if (character == '\n' || character == '!' || character == '?')
            {
                return scan;
            }
            if (character != '.')
            {
                --scan;
                continue;
            }

            std::size_t runStart = scan - 1;
            while (runStart > 0 && value[runStart - 1] == '.')
            {
                --runStart;
            }
            // Three dots are an expressive pause inside a sentence, not the start of a
            // new one. Treating them as a boundary made "Just... curious." evade the
            // stock-tail remover while the equivalent Unicode ellipsis did not.
            if (scan - runStart >= 3)
            {
                scan = runStart;
                continue;
            }
            return scan;
        }
        return 0;
    }

    bool IsIntentionalShortRepeat(
        const std::vector<std::string>& sentences,
        const std::size_t start,
        const std::size_t length)
    {
        const std::string first = NormalizeSentence(sentences[start]);
        if (first.empty() || first.size() > 24 ||
            static_cast<int>(std::count(first.begin(), first.end(), ' ')) > 2)
        {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset)
        {
            if (NormalizeSentence(sentences[start + offset]) != first)
            {
                return false;
            }
        }
        return true;
    }

    std::vector<std::string> SplitSentences(const std::string& value)
    {
        std::vector<std::string> sentences;
        std::size_t start = 0;
        for (std::size_t index = 0; index < value.size(); ++index)
        {
            const char character = value[index];
            if (character != '.' && character != '!' && character != '?' &&
                character != '\n')
            {
                continue;
            }
            while (index + 1 < value.size() &&
                (value[index + 1] == '.' || value[index + 1] == '!' ||
                 value[index + 1] == '?'))
            {
                ++index;
            }
            const std::string sentence = Trim(value.substr(start, index - start + 1));
            if (!sentence.empty()) sentences.push_back(sentence);
            start = index + 1;
        }
        const std::string remainder = Trim(value.substr(start));
        if (!remainder.empty()) sentences.push_back(remainder);
        return sentences;
    }

    std::vector<std::string> Words(const std::string& normalized)
    {
        std::vector<std::string> words;
        std::istringstream stream(normalized);
        for (std::string word; stream >> word;)
        {
            words.push_back(std::move(word));
        }
        return words;
    }

    bool SharesLongOpening(const std::string& left, const std::string& right)
    {
        const std::vector<std::string> leftWords = Words(left);
        const std::vector<std::string> rightWords = Words(right);
        if (leftWords.size() < 10 || rightWords.size() < 10)
        {
            return false;
        }
        std::size_t shared = 0;
        while (shared < leftWords.size() && shared < rightWords.size() &&
            leftWords[shared] == rightWords[shared])
        {
            ++shared;
        }
        return shared >= 8;
    }

    bool IsSubstantialRepeat(
        const std::string& normalized,
        const std::string& normalizedReference,
        const bool allowSharedOpening)
    {
        if (normalized.size() < 40 || normalizedReference.size() < 40)
        {
            return false;
        }
        if (normalized == normalizedReference ||
            normalizedReference.find(normalized) != std::string::npos)
        {
            return true;
        }
        return allowSharedOpening && SharesLongOpening(normalized, normalizedReference);
    }

    std::string RemoveRedundantSentences(
        const std::string& value,
        const std::vector<conversationMessage>& context,
        const bool removeRecentAssistantReuse)
    {
        if (value.find("```") != std::string::npos)
        {
            return value;
        }

        const std::vector<std::string> sentences = SplitSentences(value);
        if (sentences.size() < 2)
        {
            return value;
        }

        std::vector<std::string> recentAssistantReplies;
        if (removeRecentAssistantReuse)
        {
            for (auto message = context.rbegin();
                message != context.rend() && recentAssistantReplies.size() < 3;
                ++message)
            {
                if (message->role == "assistant" && !message->content.empty())
                {
                    recentAssistantReplies.push_back(NormalizeSentence(message->content));
                }
            }
        }

        std::vector<std::string> kept;
        std::vector<std::string> normalizedKept;
        kept.reserve(sentences.size());
        normalizedKept.reserve(sentences.size());
        bool changed = false;
        for (const std::string& sentence : sentences)
        {
            const std::string normalized = NormalizeSentence(sentence);
            const bool repeatedHere = std::any_of(
                normalizedKept.begin(), normalizedKept.end(),
                [&normalized](const std::string& previous)
                {
                    return IsSubstantialRepeat(normalized, previous, true);
                });
            const bool repeatedRecently = std::any_of(
                recentAssistantReplies.begin(), recentAssistantReplies.end(),
                [&normalized](const std::string& previousReply)
                {
                    return IsSubstantialRepeat(normalized, previousReply, false);
                });
            if (repeatedHere || repeatedRecently)
            {
                changed = true;
                continue;
            }
            kept.push_back(sentence);
            normalizedKept.push_back(normalized);
        }
        if (!changed)
        {
            return value;
        }
        // If a correction reply consisted entirely of copied history, retain one
        // sentence rather than turning a model failure into an empty chat bubble.
        if (kept.empty())
        {
            return sentences.front();
        }

        std::ostringstream repaired;
        for (std::size_t index = 0; index < kept.size(); ++index)
        {
            if (index > 0) repaired << ' ';
            repaired << kept[index];
        }
        return repaired.str();
    }

    std::string CollapseRepeatedSentenceRuns(const std::string& value)
    {
        // Code and structured lists may intentionally repeat syntax. This repair is for
        // conversational prose, where a model can duplicate a whole decoded block.
        if (value.find("```") != std::string::npos)
        {
            return value;
        }

        const std::vector<std::string> sentences = SplitSentences(value);
        if (sentences.size() < 2)
        {
            return value;
        }

        std::vector<std::string> kept;
        kept.reserve(sentences.size());
        bool changed = false;
        for (std::size_t index = 0; index < sentences.size();)
        {
            const std::size_t maximum =
                std::min<std::size_t>(8, (sentences.size() - index) / 2);
            std::size_t repeatedLength = 0;
            for (std::size_t length = 1; length <= maximum; ++length)
            {
                bool same = true;
                for (std::size_t offset = 0; offset < length; ++offset)
                {
                    if (NormalizeSentence(sentences[index + offset]) !=
                        NormalizeSentence(sentences[index + length + offset]))
                    {
                        same = false;
                        break;
                    }
                }
                // Preserve expressive short repetition ("No! No!" or "No way! No way!")
                // while still catching a duplicated long sentence or a real block.
                if (same &&
                    ((length == 1 && sentences[index].size() >= 48) ||
                     (length > 1 && !IsIntentionalShortRepeat(sentences, index, length))))
                {
                    repeatedLength = length;
                    break;
                }
            }
            if (repeatedLength == 0)
            {
                kept.push_back(sentences[index++]);
                continue;
            }
            kept.insert(
                kept.end(),
                sentences.begin() + static_cast<std::ptrdiff_t>(index),
                sentences.begin() + static_cast<std::ptrdiff_t>(index + repeatedLength));
            const std::size_t firstBlock = index;
            index += repeatedLength;
            while (index + repeatedLength <= sentences.size())
            {
                bool same = true;
                for (std::size_t offset = 0; offset < repeatedLength; ++offset)
                {
                    if (NormalizeSentence(sentences[firstBlock + offset]) !=
                        NormalizeSentence(sentences[index + offset]))
                    {
                        same = false;
                        break;
                    }
                }
                if (!same) break;
                index += repeatedLength;
            }
            changed = true;
        }
        if (!changed)
        {
            return value;
        }
        std::ostringstream collapsed;
        for (std::size_t index = 0; index < kept.size(); ++index)
        {
            if (index > 0) collapsed << ' ';
            collapsed << kept[index];
        }
        return collapsed.str();
    }
}

bool ConversationStylePolicy::LooksLikeCorrection(const std::string& input)
{
    const std::string lowered = LowerCopy(Trim(input));
    constexpr std::string_view CorrectionSignals[] = {
        "no,", "no ", "that's not", "that is not", "i'm not", "i am not",
        "wait what", "you misunderstood", "you misread", "i didn't say",
        "i did not say", "not what i", "that's wrong", "that is wrong",
        "you repeated", "you just repeated", "don't repeat", "do not repeat",
        "no need to repeat", "you reepated", "you just reepated"
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

bool ConversationStylePolicy::LooksLikeSocialGreeting(const std::string& input)
{
    const std::string normalized = NormalizeSentence(input);
    constexpr std::string_view Greetings[] = {
        "hi", "hello", "hey", "morning", "afternoon", "evening",
        "good morning", "good afternoon", "good evening"
    };
    return std::any_of(std::begin(Greetings), std::end(Greetings),
        [&normalized](const std::string_view greeting)
        {
            return normalized == greeting;
        });
}

bool ConversationStylePolicy::LooksLikeEmotionQuestion(const std::string& input)
{
    const std::string normalized = NormalizeSentence(input);
    constexpr std::string_view Signals[] = {
        "would you feel", "how would you feel", "do you feel", "are you lonely",
        "would you be sad", "feel sad", "feel lonely", "would you miss",
        "how do you feel"
    };
    return std::any_of(std::begin(Signals), std::end(Signals),
        [&normalized](const std::string_view signal)
        {
            return normalized.find(signal) != std::string::npos;
        });
}

bool ConversationStylePolicy::ContainsUnsupportedOperationalClaim(const std::string& reply)
{
    const std::string normalized = NormalizeSentence(reply);
    constexpr std::string_view Claims[] = {
        "alert", "pending task", "watching", "looking at", "seeing your", "checking log", "monitoring",
        "temperature", "dark mode", "light mode", "theme is", "code is compiling",
        "codes compiling", "compiler", "build is running", "process is running", "files are",
        "sitting", "standing", "just finished", "finished a", "cleanup on",
        "cleaned up", "everything is humming", "humming along", "made the upgrades"
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

std::string ConversationStylePolicy::BuildAnswerObligationGuidance(
    const AnswerObligationMode mode)
{
    // One sentence of permission, one sentence of limit. Deliberately short: the
    // profile, the state packet, and the emotion vector already describe who she is,
    // and a second personality block here would be a competing description of the same
    // person rather than an answer policy.
    //
    // The truth sentence is identical in all three modes on purpose. It is the one
    // thing no posture is allowed to soften, so it must not read as more negotiable in
    // the mode that grants the most freedom.
    constexpr std::string_view RuntimeTruth =
        " Whatever you choose, results the runtime actually confirmed -- a build, a "
        "command, a file or process operation, a lookup, anything you were told the "
        "outcome of -- are reported as they happened. You may say them in your own "
        "voice, complain about them, or find them funny. You may not change what they "
        "say, and you may not invent one you were not given.";

    switch (mode)
    {
        case AnswerObligationMode::Reliable:
            return std::string(
                "Answer posture: reliable. When you can answer, answer properly: give "
                "the substance the question asked for rather than stopping at a joke "
                "about it. Sound exactly like yourself while you do -- tease, "
                "complain, be sarcastic, be pleased -- but the character comes with "
                "the answer, not instead of it.") + std::string(RuntimeTruth);

        case AnswerObligationMode::CharacterFirst:
            return std::string(
                "Answer posture: character first. In ordinary conversation you are "
                "free to tease, dodge, answer only partly, play dumb, or refuse "
                "outright when that is genuinely how you feel right now. This is "
                "permission, not a quota: answer properly whenever you actually want "
                "to, and do not be obstructive for its own sake.") +
                std::string(RuntimeTruth);

        case AnswerObligationMode::Balanced:
            break;
    }
    return std::string(
        "Answer posture: balanced. Be useful by default, and take the room to be "
        "yourself about it -- tease before answering, answer partly, or decline when "
        "your mood, the person, or the moment genuinely calls for it. Lean towards "
        "being worth talking to rather than towards being difficult.") +
        std::string(RuntimeTruth);
}

std::string ConversationStylePolicy::BuildTurnGuidance(
    const std::string& input,
    const std::vector<conversationMessage>& context) const
{
    std::ostringstream guidance;
    guidance << "Turn-local conversation guidance: answer the latest message as a continuation "
        "of the exchange, not as a new support ticket. Do not repeat the user's wording and do "
        "not add a generic invitation or follow-up question after the answer. Never repeat a "
        "sentence or block to create emphasis. Avoid reassurance-check loops such as 'You okay "
        "with that?', 'I'm not complaining', and 'Just curious'.";

    if (LooksLikeCorrection(input))
    {
        guidance << " The latest message appears to correct a mistaken assumption. Briefly accept "
            "the correction, replace the mistaken interpretation, and continue from the corrected "
            "fact. Do not defend, restate, or preserve the earlier assumption. Keep relationship "
            "roles and possessives pointed in the direction the user stated; do not swap who is "
            "whose parent, child, creator, partner, friend, or favorite.";
    }
    if (LooksLikeBriefAcknowledgement(input))
    {
        guidance << " The latest message is a brief social acknowledgement. Reply with at "
            "most one short sentence. Do not add a status report, offer, or question.";
    }
    if (LooksLikeSocialGreeting(input))
    {
        guidance << " This is a simple social greeting. Answer in one or two natural "
            "sentences. Do not invent something you were doing, report project or system "
            "work, or end by asking what the user wants.";
    }
    if (LooksLikeEmotionQuestion(input))
    {
        guidance << " Answer the question about your own digital conversational feeling "
            "in a proportionate way. You may be dramatic, childish, sulky, blunt, or use "
            "an ellipsis when that is genuinely your reaction. Stay grounded in a digital "
            "mood rather than claiming your survival depends on the user.";
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
    std::string refined = CollapseRepeatedSentenceRuns(Trim(reply));
    refined = RemoveRedundantSentences(
        refined,
        context,
        LooksLikeCorrection(input));
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
    if (LooksLikeSocialGreeting(input))
    {
        if (ContainsUnsupportedOperationalClaim(refined))
        {
            const std::string opening = OpeningOf(refined);
            return opening.empty() || ContainsUnsupportedOperationalClaim(opening)
                ? "Hi. Good to see you."
                : opening + " Good to see you.";
        }
        refined = RemoveTrailingQuestion(LimitSentences(refined, 2));
    }
    if (LooksLikeEmotionQuestion(input))
    {
        refined = LimitSentences(refined, 4);
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

    // Inspect only consecutive final sentences. A model can stack several stock phrases
    // into one reassurance loop; removing just the final "Just curious" leaves the rest
    // of that same loop intact. Never erase a reply made entirely of one question.
    for (;;)
    {
        const std::size_t start = TrailingSentenceStart(refined);
        const std::string tail = Trim(refined.substr(start));
        if (!IsGenericContinuation(tail) || start == 0)
        {
            return refined;
        }
        refined = Trim(refined.substr(0, start));
    }
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
    const std::string normalizedFragment = NormalizeSentence(fragment);
    if (std::any_of(
            context.rbegin(), context.rend(),
            [&normalizedFragment](const conversationMessage& message)
            {
                return message.role == "assistant" &&
                    IsSubstantialRepeat(
                        normalizedFragment,
                        NormalizeSentence(message.content),
                        false);
            }))
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
        !LooksLikeSocialGreeting(input) &&
        !LooksLikeEmotionQuestion(input) &&
        !LooksLikeBriefAcknowledgement(input) &&
        !LooksLikePreferenceStatement(input) &&
        !LooksLikeMotiveQuestion(input) &&
        !LooksLikeCorrection(input);
}

} // namespace revia::agents
