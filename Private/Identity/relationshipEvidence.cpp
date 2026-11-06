#include "Identity/relationshipEvidence.h"
#include "Core/speechAttribution.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace revia::identity
{

namespace
{
    bool NamingIsNegated(const std::string& input, const std::size_t at)
    {
        // Only a directly negated naming occurrence is excluded. A negation elsewhere
        // ("don't forget that my name is Alex") does not negate the introduction.
        const std::size_t begin = at > 64 ? at - 64 : 0;
        std::string lead;
        for (std::size_t index = begin; index < at; ++index)
        {
            const unsigned char ch = static_cast<unsigned char>(input[index]);
            if (std::isspace(ch) != 0)
            {
                if (!lead.empty() && lead.back() != ' ') lead.push_back(' ');
            }
            else lead.push_back(static_cast<char>(std::tolower(ch)));
        }
        while (!lead.empty() && lead.back() == ' ') lead.pop_back();
        for (std::size_t quote = lead.find("\xE2\x80\x99"); quote != std::string::npos;
            quote = lead.find("\xE2\x80\x99", quote + 1))
            lead.replace(quote, 3, "'");
        // "Why not call me Alex?" is a suggestion, not a rejected name.
        if (lead == "why not" || lead.ends_with(" why not")) return false;
        for (const std::string_view suffix : {"not", "never", "don't", "dont", "cannot", "can't", "cant",
                 "mustn't", "shouldn't", "wouldn't", "won't", "don't ever", "do not ever", "never ever",
                 "not to", "don't want you to", "do not want you to", "don't want to", "do not want to"})
        {
            if (!lead.ends_with(suffix)) continue;
            const auto start = lead.size() - suffix.size();
            if (start == 0 || (std::isalnum(static_cast<unsigned char>(lead[start - 1])) == 0 && lead[start - 1] != '_'))
                return true;
        }
        return false;
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

    bool ContainsAny(const std::string& haystack, const std::initializer_list<std::string_view> needles)
    {
        return std::any_of(needles.begin(), needles.end(),
            [&haystack](const std::string_view needle)
            {
                return haystack.find(needle) != std::string::npos;
            });
    }

    bool ContainsAnyWholePhrase(const std::string& haystack, const std::initializer_list<std::string_view> needles)
    {
        const auto isWordCharacter = [](const unsigned char character) { return std::isalnum(character) != 0 || character == '_'; };
        return std::any_of(needles.begin(), needles.end(),
            [&haystack, &isWordCharacter](const std::string_view needle)
            {
                std::size_t at = haystack.find(needle);
                while (at != std::string::npos)
                {
                    const std::size_t end = at + needle.size();
                    if ((at == 0 || !isWordCharacter(static_cast<unsigned char>(haystack[at - 1]))) &&
                        (end == haystack.size() || !isWordCharacter(static_cast<unsigned char>(haystack[end]))))
                        return true;
                    at = haystack.find(needle, at + 1);
                }
                return false;
            });
    }

    bool HasWrongResultComplaint(const std::string& input)
    {
        constexpr std::string_view prefix = "you keep getting ";
        for (std::size_t at = input.find(prefix); at != std::string::npos; at = input.find(prefix, at + prefix.size()))
        {
            if (at > 0 && (std::isalnum(static_cast<unsigned char>(input[at - 1])) != 0 || input[at - 1] == '_')) continue;
            const auto begin = at + prefix.size();
            const auto punctuation = input.find_first_of(".,!?;:\r\n", begin);
            const auto end = punctuation == std::string::npos ? input.size() : punctuation;
            // Read one bounded result clause, not an unrelated later use of "wrong".
            if (end - begin > 128) continue;
            std::string_view result(input.data() + begin, end - begin);
            while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back())) != 0) result.remove_suffix(1);
            if (result.ends_with(" again")) result.remove_suffix(6);
            if (!result.ends_with(" wrong")) continue;
            result.remove_suffix(6);
            while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back())) != 0) result.remove_suffix(1);
            if (result.empty() || result == "not" || result == "never" || result.ends_with(" not") || result.ends_with(" never") ||
                result.ends_with(" instead of") || result.ends_with(" rather than")) continue;
            // Only a simple object phrase: praise or another action/relative clause
            // ending in "wrong" does not establish that she got the object wrong.
            if (ContainsAnyWholePhrase(std::string(result), {"better at", "good at", "and", "or", "but", "which", "what", "who"})) continue;
            return true;
        }
        return false;
    }

    bool IsDirectRetryRequest(const std::string& input)
    {
        std::string request;
        request.reserve(input.size());
        for (const unsigned char character : input)
        {
            if (std::isspace(character) != 0)
            {
                if (!request.empty() && request.back() != ' ') request.push_back(' ');
            }
            else request.push_back(static_cast<char>(character));
        }
        const auto begin = request.find_first_not_of(" .!?()[]");
        if (begin == std::string::npos) return false;
        const auto end = request.find_last_not_of(" .!?()[]");
        request = request.substr(begin, end - begin + 1);
        // A rejected answer may precede the same direct retry: "No, try again."
        for (const std::string_view rejection : {"no", "nope"})
        {
            if (!request.starts_with(rejection) || request.size() == rejection.size()) continue;
            const auto next = static_cast<unsigned char>(request[rejection.size()]);
            if (std::isalnum(next) != 0 || next == '_') continue;
            const auto retry = request.find_first_not_of(" ,.!?;:()[]", rejection.size());
            if (retry == std::string::npos) return false;
            request.erase(0, retry);
            break;
        }
        if (request == "again") return true;
        // An explicit check-then-retry instruction is still a retry, not ordinary recurrence.
        if (request.starts_with("check ") && ContainsAnyWholePhrase(request, {"then try again"})) return true;
        for (const std::string_view prefix :
            {"try again", "please try again", "can you try again", "could you try again", "would you try again"})
        {
            if (!request.starts_with(prefix)) continue;
            if (request.size() == prefix.size() ||
                (std::isalnum(static_cast<unsigned char>(request[prefix.size()])) == 0 && request[prefix.size()] != '_'))
                return true;
        }
        return false;
    }

    bool NegatesJudgmentBefore(const std::string& original, const std::size_t at)
    {
        // Use original text at the authored occurrence's offset. Masked quotations
        // must not join a prior "I'm not saying" to a separate unquoted insult.
        const std::size_t begin = at > 64 ? at - 64 : 0;
        std::string lead;
        for (std::size_t index = begin; index < at; ++index)
        {
            const unsigned char ch = static_cast<unsigned char>(original[index]);
            if (std::isspace(ch) != 0)
            {
                if (!lead.empty() && lead.back() != ' ') lead.push_back(' ');
            }
            else lead.push_back(static_cast<char>(std::tolower(ch)));
        }
        while (!lead.empty() && lead.back() == ' ') lead.pop_back();
        for (std::size_t quote = lead.find("\xE2\x80\x99"); quote != std::string::npos;
            quote = lead.find("\xE2\x80\x99", quote + 1))
            lead.replace(quote, 3, "'");
        if (lead.ends_with(" that")) lead.resize(lead.size() - 5);
        for (const std::string_view prefix :
            {"i don't think", "i do not think", "i'm not saying", "i am not saying"})
        {
            if (!lead.ends_with(prefix)) continue;
            const auto start = lead.size() - prefix.size();
            if (start == 0 || (std::isalnum(static_cast<unsigned char>(lead[start - 1])) == 0 && lead[start - 1] != '_'))
                return true;
        }
        return false;
    }
    }

    ConversationSignals ReadConversationSignals(const std::string& userInput, const std::string& reply, const bool succeeded)
    {
        ConversationSignals signals;
        signals.userInput = userInput;
        signals.reply = reply;
        signals.succeeded = succeeded;

        const std::string input = Lower(conversation::ReadSpeechAttribution(userInput).userAuthoredText);
        signals.explicitlyPlayful = ContainsAny(input, {"just kidding", "only kidding", "i'm joking", "i am joking", "just teasing"});

        // Aimed at Revia, not at the problem. The distinction matters: "this is broken" is
        // not evidence about the relationship, and treating it as such would make her resent
        // anyone doing difficult work with her.
        for (const std::string_view phrase :
            {"i hate you", "you're useless", "you are useless", "you're stupid", "you are stupid", "shut up",
                "nobody likes you", "you suck", "you're worthless", "you are worthless"})
        {
            for (std::size_t at = input.find(phrase); at != std::string::npos;
                at = input.find(phrase, at + phrase.size()))
            {
                if (NegatesJudgmentBefore(userInput, at)) continue;
                signals.hostileTowardRevia = true;
                break;
            }
            if (signals.hostileTowardRevia) break;
        }

        // A direct put-down is different from the user discussing their own care, or
        // quoting somebody else's words. Keep this narrow: the word therapy by itself
        // carries no hostility, and a concerned question is not a taunt.
        const auto start = input.find_first_not_of(" \t\r\n");
        const std::string direct = start == std::string::npos ? "" : input.substr(start);
        for (const std::string_view opener :
            {"you need therapy", "revia, you need therapy", "you need an exorcism", "revia, you need an exorcism"})
        {
            if (direct.starts_with(opener) &&
                (direct.size() == opener.size() || std::string_view(".! ,").find(direct[opener.size()]) != std::string_view::npos) &&
                direct.find('?') == std::string::npos &&
                !ContainsAny(direct, {"i'm worried", "i am worried", "i care about", "because i'm concerned"}))
            {
                signals.hostileTowardRevia = true;
            }
        }

        signals.expressedAppreciation = ContainsAny(
            input, {"thank", "thanks", "appreciate", "well done", "good job", "nice work", "that helped", "perfect", "exactly right"});

        // Recurrence and praise ("good to see you again", "you keep making me laugh")
        // do not establish a failed answer. Keep explicit complaints and direct retries;
        // never let a positive clause suppress a separate correction in the same turn.
        signals.repeatedCorrection = ContainsAnyWholePhrase(input,
            {"i already said", "i just said", "still not", "that's not what", "that is not what", "no, i meant",
                "you keep missing", "you keep forgetting", "you keep misreading", "you keep misunderstanding",
                "you keep repeating yourself", "you keep getting it wrong", "you keep getting that wrong", "you keep making me repeat"}) ||
            IsDirectRetryRequest(input) || HasWrongResultComplaint(input);

        signals.collaborative =
            succeeded && ContainsAny(input, {"let's", "lets ", "we should", "can you help", "work on", "together", "figure out", "let us"});

        // A short acknowledgement is not a relationship event. Weighting every "ok" the same
        // as a real exchange would let idle chatter accumulate into closeness.
        const bool substantial = userInput.size() > 24;
        signals.importance = substantial ? 0.45F : 0.2F;
        if (signals.hostileTowardRevia || signals.expressedAppreciation)
        {
            // Something was said about her specifically, which counts for more either way.
            signals.importance = 0.7F;
        }
        return signals;
    }

namespace
{
    // "this is X" and "name's X" read as introductions only where a person would
    // introduce themselves: at the start of what they said, after at most a greeting.
    // Anywhere else they are ordinary sentences -- "this is where it breaks", "this is
    // not right", "this is part of the build" -- and matching them renamed the local
    // user to "Where", "Not", and "Part".
    bool StartsTheUtterance(const std::string& lowered, const std::size_t at)
    {
        static const std::vector<std::string_view> greetings = {
            "hi", "hey", "hello", "hiya", "yo", "revia", "hi revia", "hey revia",
            "hello revia", "oh", "ok", "okay", "so", "well"
        };
        std::size_t start = 0;
        while (start < at && std::string_view(" \t\r\n,.!-").find(lowered[start]) !=
            std::string_view::npos) ++start;
        if (start == at) return true;
        std::string lead = lowered.substr(start, at - start);
        while (!lead.empty() &&
            std::string_view(" \t\r\n,.!-").find(lead.back()) != std::string_view::npos)
            lead.pop_back();
        // A greeting must be followed by punctuation or a space and nothing else.
        return std::find(greetings.begin(), greetings.end(), lead) != greetings.end();
    }

    // Rejecting these matters: "call me later" and "this is fine" both match an opener,
    // and storing them would rename the person to a stray word.
    bool IsNotAName(const std::string& lowered)
    {
        static const std::vector<std::string> notNames = {"later", "back", "when", "if", "fine", "good", "it", "the", "a", "an", "me",
            "you", "your", "my", "and", "but", "so", "that", "this", "what", "where", "why", "how", "who", "which", "not", "part", "just",
            "really", "very", "all", "about", "like", "going", "gonna", "getting", "what's", "it's", "because", "now", "here", "there",
            "then", "too", "also", "only", "still", "actually", "definitely", "probably", "literally", "great", "cool", "nice", "awesome",
            "amazing", "weird", "strange", "crazy", "wild", "wrong", "right", "true", "false", "bad", "worse", "better", "best", "hard",
            "easy", "interesting", "important", "funny", "annoying", "broken", "working", "done", "new", "old", "big", "small", "our",
            "his", "her", "their", "its", "some", "any", "no", "one", "exactly", "insane", "revia", "anytime", "tomorrow", "tonight",
            "whenever", "sometime", "again", "maybe"};
        return std::find(notNames.begin(), notNames.end(), lowered) != notNames.end();
    }

    // The speaker's own words, quotes excluded, as lowered word lists per clause.
    std::vector<std::vector<std::string>> AuthoredClauses(const std::string& userInput, const std::string_view boundaries)
    {
        std::string authored = Lower(conversation::ReadSpeechAttribution(userInput).userAuthoredText);
        for (std::size_t at = authored.find("\xE2\x80\x99"); at != std::string::npos; at = authored.find("\xE2\x80\x99", at))
            authored.replace(at, 3, "'");
        std::vector<std::vector<std::string>> clauses(1);
        std::string word;
        for (const char character : authored + '.')
        {
            if (std::isalpha(static_cast<unsigned char>(character)) != 0 || character == '\'' || character == '-')
            {
                word.push_back(character);
                continue;
            }
            if (!word.empty())
                clauses.back().push_back(std::move(word));
            word.clear();
            if (boundaries.find(character) != std::string_view::npos && !clauses.back().empty())
                clauses.emplace_back();
        }
        if (clauses.back().empty())
            clauses.pop_back();
        return clauses;
    }

    bool IsExplicitNameCorrection(const std::string& input, const std::size_t at, const std::size_t end)
    {
        // Require a complete final introduction; a conditional, example or joke tail
        // does not establish a replacement identity.
        if (input.find_first_not_of(" \t\r\n.!", end) != std::string::npos) return false;
        std::size_t before = at;
        while (before > 0 && std::isspace(static_cast<unsigned char>(input[before - 1])) != 0) --before;
        for (const std::string_view marker : {"actually,", "no,"})
        {
            if (before < marker.size()) continue;
            std::size_t begin = before - marker.size();
            if (Lower(input.substr(begin, marker.size())) != marker) continue;
            while (begin > 0 && std::isspace(static_cast<unsigned char>(input[begin - 1])) != 0) --begin;
            // Original bytes retain quotation boundaries that the authored view masks.
            return begin == 0 || std::string_view(".!?;").find(input[begin - 1]) != std::string_view::npos;
        }
        return false;
    }
}

namespace
{
// includeArrival: also "this is X", which announces someone arriving rather than naming oneself.
std::string ReadStatedNameFrom(const std::string& userInput, const bool includeArrival)
{
    struct Opener
    {
        std::string_view text;
        bool onlyAtStart;
    };
    static const std::vector<Opener> openers = {
        {"my name is ", false}, {"you can call me ", false}, {"call me ", false},
        {"i am called ", false}, {"i'm called ", false},
        {"name's ", true}, {"this is ", true}
    };
    const std::string lowered = Lower(conversation::ReadSpeechAttribution(userInput).userAuthoredText);
    for (const Opener& opener : openers)
    {
        if (!includeArrival && opener.text == "this is ")
            continue;
        std::string stated;
        // A rejected match must not hide a later introduction using the same phrase.
        // Keep opener priority and the first valid match unless explicitly corrected.
        for (std::size_t at = lowered.find(opener.text); at != std::string::npos;
            at = lowered.find(opener.text, at + opener.text.size()))
        {
            // The authored copy chooses the occurrence. Original-text lookbehind keeps
            // quote boundaries rather than joining a masked quote onto a prior "not".
            if (NamingIsNegated(userInput, at)) continue;
            // Whole words only: "recall me" is not "call me", "surname's" is not "name's".
            if (at > 0 && std::isalpha(static_cast<unsigned char>(lowered[at - 1])) != 0)
            {
                continue;
            }
            if (opener.onlyAtStart && !StartsTheUtterance(lowered, at))
            {
                // A later occurrence cannot move back to the start of the utterance.
                break;
            }
            std::size_t index = at + opener.text.size();
            std::string name;
            // One word, letters and a couple of joining characters only. Taking the rest of
            // the sentence would store "quentin and i work on revia" as somebody's name.
            while (index < userInput.size() && name.size() < 32)
            {
                const unsigned char character = static_cast<unsigned char>(userInput[index]);
                if (std::isalpha(character) != 0 || character == '-' || character == '\'')
                {
                    name.push_back(userInput[index]);
                    ++index;
                    continue;
                }
                break;
            }
            if (name.size() < 2)
            {
                continue;
            }
            if (IsNotAName(Lower(name)))
            {
                continue;
            }
            name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            if (stated.empty() || IsExplicitNameCorrection(userInput, at, index))
                stated = name;
        }
        if (!stated.empty()) return stated;
    }
    return {};
}
}

std::string ReadStatedName(const std::string& userInput)
{
    return ReadStatedNameFrom(userInput, true);
}

bool NamesThemselves(const std::string& userInput)
{
    return !ReadStatedNameFrom(userInput, false).empty();
}

std::string ReadSpeakerNameCorrection(const std::string& userInput, const std::string& currentName)
{
    const std::string current = Lower(currentName);
    if (current.empty())
        return {};
    // The contrast must reject the exact name in use, and the new name must follow a
    // self-reference: "I meant", "I said", "I'm", "it's". "Send it to Ana, not Jo" and
    // "I told Ana, not Jo" are about other people.
    for (const auto& words : AuthoredClauses(userInput, ".!?;\n"))
    {
        for (std::size_t index = 1; index + 2 < words.size(); ++index)
        {
            if (words[index + 1] != "not" || words[index + 2] != current || words[index] == current)
                continue;
            const std::string& frame = words[index - 1];
            const std::string before = index >= 2 ? words[index - 2] : std::string();
            const bool selfReference = frame == "im" || frame == "i'm" || frame == "its" || frame == "it's" ||
                                       ((frame == "meant" || frame == "said" || frame == "am") && before == "i") ||
                                       (frame == "is" && before == "it");
            if (selfReference && words[index].size() >= 2 && !IsNotAName(words[index]))
            {
                std::string corrected = words[index];
                corrected[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(corrected[0])));
                return corrected;
            }
        }
    }
    return {};
}

std::string ReadReturningSpeaker(const std::string& userInput, const std::vector<std::string>& knownNames)
{
    const auto known = [&knownNames](const std::string& word) -> std::string
    {
        for (const auto& name : knownNames)
            if (!name.empty() && Lower(name) == word)
                return name;
        return {};
    };
    static const std::vector<std::string_view> greetings = {"hey", "hi", "hello", "hiya", "yo", "oh", "ok", "okay", "so", "well", "revia"};
    auto clauses = AuthoredClauses(userInput, ",.!?;\n");
    for (auto& clause : clauses)
        while (!clause.empty() && std::find(greetings.begin(), greetings.end(), clause.front()) != greetings.end())
            clause.erase(clause.begin());
    std::erase_if(clauses, [](const auto& clause) { return clause.empty(); });
    if (clauses.empty())
        return {};
    // Only the opening clause introduces someone: "it's Mari", "I'm Mari again",
    // "Mari here" or "it's me, Mari". "I'm Mari's cousin" names nobody present.
    auto first = clauses.front();
    const bool itsMe = first == std::vector<std::string>{"it's", "me"} || first == std::vector<std::string>{"its", "me"} ||
                       first == std::vector<std::string>{"it", "is", "me"};
    if (itsMe)
        return clauses.size() > 1 && clauses[1].size() == 1 ? known(clauses[1][0]) : std::string();
    if (first.size() == 2 && first.back() == "here")
        return known(first.front());
    if (first.size() >= 3 && (first.back() == "again" || first.back() == "here"))
        first.pop_back();
    if (first.size() == 2 && (first[0] == "it's" || first[0] == "its" || first[0] == "i'm" || first[0] == "im"))
        return known(first[1]);
    if (first.size() == 3 && ((first[0] == "it" && first[1] == "is") || (first[0] == "i" && first[1] == "am")))
        return known(first[2]);
    return {};
}

RelationshipEvent BuildRelationshipEvent(const std::string& entityId, const ConversationSignals& signals)
{
    RelationshipEvent event;
    event.entityId = entityId;
    event.importance = std::clamp(signals.importance, 0.0F, 1.0F);
    // Read from keywords, not measured. The registry scales every delta by this, so an
    // inference the reader is unsure of barely moves anything.
    event.confidence = 0.55F;

    if (signals.expressedAppreciation)
    {
        event.positiveInteraction = 0.7F;
        // Being thanked is evidence that what she said held up, which is what trust is
        // actually made of.
        event.trustEvidence = 0.35F;
        event.description = "expressed appreciation";
    }
    if (signals.collaborative)
    {
        event.cooperation = std::max(event.cooperation, 0.6F);
        event.positiveInteraction = std::max(event.positiveInteraction, 0.4F);
        if (event.description.empty()) event.description = "worked on something together";
    }
    if (signals.hostileTowardRevia)
    {
        event.negativeInteraction = 0.8F;
        // Only hostility aimed at her counts as disrespect. This is the one signal that
        // accrues as lasting grievance rather than passing friction.
        event.disrespectEvidence = 0.5F;
        event.conflict = 0.5F;
        event.description = "a remark aimed at her";
    }
    else if (signals.repeatedCorrection)
    {
        // Friction, not disrespect. Being corrected repeatedly is her failing to listen,
        // and holding a grudge about being told so would be exactly backwards.
        event.conflict = 0.35F;
        event.negativeInteraction = 0.25F;
        if (event.description.empty()) event.description = "had to repeat themselves";
    }
    if (!signals.succeeded)
    {
        // The turn itself went wrong. That is not the user's fault and must not read as
        // conflict; it only dampens the positive side.
        event.positiveInteraction *= 0.5F;
        event.cooperation *= 0.5F;
    }
    if (event.description.empty())
    {
        event.description = "an ordinary exchange";
    }
    return event;
}

} // namespace revia::identity
