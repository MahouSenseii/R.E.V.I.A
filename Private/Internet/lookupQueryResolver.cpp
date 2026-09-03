#include "Internet/lookupQueryResolver.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <vector>

namespace revia::internet
{

namespace
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string Trim(std::string value)
{
    const auto notSpace = [](const unsigned char character)
    {
        return std::isspace(character) == 0;
    };
    const auto first = std::find_if(value.begin(), value.end(), notSpace);
    if (first == value.end()) return {};
    const auto last = std::find_if(value.rbegin(), value.rend(), notSpace).base();
    return std::string(first, last);
}

// Only the characters that end a sentence. Deliberately not every punctuation mark:
// "std::expected", "Qwen3-TTS" and "0xC0000135" all end in or contain punctuation that
// is part of the term.
std::string TrimSentencePunctuation(std::string value)
{
    const auto isSentenceEdge = [](const unsigned char character)
    {
        return character == '?' || character == '!' || character == '.' ||
            character == ',' || character == ';' || std::isspace(character) != 0;
    };
    while (!value.empty() &&
        isSentenceEdge(static_cast<unsigned char>(value.back())))
    {
        // A trailing '.' can belong to a version number, so it only goes if what comes
        // before it is not a digit: "Python 3.14." loses the stop, "3.14" keeps it.
        if (value.back() == '.' && value.size() >= 2 &&
            std::isdigit(static_cast<unsigned char>(value[value.size() - 2])) != 0)
        {
            break;
        }
        value.pop_back();
    }
    return Trim(std::move(value));
}

// True when `phrase` sits at the front of `lowered` as whole words.
//
// The boundary check is the whole reason this is not a substring search. "search for"
// is a prefix of "search formatting options", and stripping it there would leave
// "matting options".
bool StartsWithPhrase(const std::string& lowered, const std::string_view phrase)
{
    if (lowered.size() < phrase.size()) return false;
    if (lowered.compare(0, phrase.size(), phrase) != 0) return false;
    if (lowered.size() == phrase.size()) return true;
    const unsigned char next = static_cast<unsigned char>(lowered[phrase.size()]);
    return std::isalnum(next) == 0;
}

bool EndsWithPhrase(const std::string& lowered, const std::string_view phrase)
{
    if (lowered.size() < phrase.size()) return false;
    const std::size_t at = lowered.size() - phrase.size();
    if (lowered.compare(at, phrase.size(), phrase) != 0) return false;
    if (at == 0) return true;
    const unsigned char before = static_cast<unsigned char>(lowered[at - 1]);
    return std::isalnum(before) == 0;
}

// Politeness and framing that comes before the actual instruction.
constexpr std::array LeadingCourtesies{
    std::string_view{"hey revia"}, std::string_view{"ok revia"},
    std::string_view{"okay revia"}, std::string_view{"revia"},
    std::string_view{"i want you to"}, std::string_view{"i would like you to"},
    std::string_view{"i'd like you to"}, std::string_view{"do me a favor and"},
    std::string_view{"do me a favour and"},
    std::string_view{"can you"}, std::string_view{"could you"},
    std::string_view{"would you"}, std::string_view{"will you"},
    std::string_view{"please"}, std::string_view{"hey"}, std::string_view{"hi"},
    std::string_view{"go"}, std::string_view{"and"},
};

// The instruction itself. Every entry either carries an explicit object marker ("for",
// "about") or is unambiguous as a command.
//
// Bare "search" and bare "lookup" are deliberately absent. "search algorithm
// complexity" and "lookup table performance C++" are subjects, not commands, and a
// resolver that cannot tell the difference is worse than one that leaves the sentence
// alone.
constexpr std::array LookupVerbs{
    std::string_view{"search the web for"}, std::string_view{"search online for"},
    std::string_view{"search the internet for"}, std::string_view{"search the web"},
    std::string_view{"check the web for"}, std::string_view{"check online for"},
    std::string_view{"check the internet for"},
    std::string_view{"look up"}, std::string_view{"look this up"},
    std::string_view{"look it up"}, std::string_view{"look that up"},
    std::string_view{"look into"}, std::string_view{"look online for"},
    std::string_view{"search for"}, std::string_view{"google"},
    // "research" is here and bare "search" is not, and the asymmetry is deliberate.
    // "search algorithm complexity" is a real subject, so removing a bare "search"
    // would destroy it. "research methodology" is the only comparable risk for this
    // one, and it narrows a query rather than losing the subject, which is the better
    // failure of the two -- without it "Research anything" searches for the word
    // "anything".
    std::string_view{"research"},
    std::string_view{"find out about"}, std::string_view{"find out"},
    std::string_view{"read about"}, std::string_view{"read up on"},
    std::string_view{"tell me about"}, std::string_view{"find me"},
    std::string_view{"check online"}, std::string_view{"check whether"},
    std::string_view{"check if"}, std::string_view{"check when"},
    std::string_view{"check"},
};

// Filler that survives verb removal and adds nothing to a query.
constexpr std::array LeadingFiller{
    std::string_view{"whether"}, std::string_view{"if"}, std::string_view{"when"},
    std::string_view{"the"}, std::string_view{"a"}, std::string_view{"an"},
    std::string_view{"about"}, std::string_view{"for"}, std::string_view{"on"},
    std::string_view{"me"}, std::string_view{"us"},
};

// Conversational tail that belongs to the request, not to the subject.
constexpr std::array TrailingCourtesies{
    std::string_view{"for me"}, std::string_view{"for us"},
    std::string_view{"please"}, std::string_view{"thanks"},
    std::string_view{"thank you"}, std::string_view{"if you can"},
    std::string_view{"if you could"}, std::string_view{"would you"},
    std::string_view{"can you"}, std::string_view{"could you"},
};

// Phrases that hand the choice of subject back instead of naming one. Searching any of
// these returns results about the words themselves, which is how a delegation becomes
// confident nonsense in the grounding block.
constexpr std::array Delegations{
    std::string_view{"whatever you want"}, std::string_view{"what you want"},
    std::string_view{"whatever you like"}, std::string_view{"anything you want"},
    std::string_view{"whatever you find interesting"},
    std::string_view{"something interesting"}, std::string_view{"something cool"},
    std::string_view{"anything interesting"},
    std::string_view{"whatever"}, std::string_view{"anything"},
    std::string_view{"something"}, std::string_view{"you decide"},
    std::string_view{"your choice"}, std::string_view{"up to you"},
    std::string_view{"stuff"}, std::string_view{"things"},
    std::string_view{"it"}, std::string_view{"that"}, std::string_view{"this"},
    std::string_view{"one"}, std::string_view{"them"},
};

// Words that cannot carry a subject on their own.
constexpr std::array Stopwords{
    std::string_view{"the"}, std::string_view{"and"}, std::string_view{"or"},
    std::string_view{"of"}, std::string_view{"for"}, std::string_view{"to"},
    std::string_view{"in"}, std::string_view{"on"}, std::string_view{"about"},
    std::string_view{"with"}, std::string_view{"you"}, std::string_view{"me"},
    std::string_view{"my"}, std::string_view{"your"}, std::string_view{"want"},
    std::string_view{"like"}, std::string_view{"please"}, std::string_view{"some"},
    std::string_view{"any"}, std::string_view{"what"}, std::string_view{"whatever"},
    std::string_view{"it"}, std::string_view{"its"}, std::string_view{"that"},
    std::string_view{"this"}, std::string_view{"one"}, std::string_view{"them"},
    std::string_view{"they"}, std::string_view{"out"}, std::string_view{"yet"},
    std::string_view{"has"}, std::string_view{"have"}, std::string_view{"had"},
    std::string_view{"is"}, std::string_view{"are"}, std::string_view{"was"},
    std::string_view{"were"}, std::string_view{"been"}, std::string_view{"still"},
};

bool IsStopword(const std::string& word)
{
    return std::find(Stopwords.begin(), Stopwords.end(), word) != Stopwords.end();
}

std::vector<std::string> Words(const std::string& value)
{
    std::vector<std::string> words;
    std::string current;
    for (const unsigned char character : value)
    {
        if (std::isalnum(character) != 0)
        {
            current.push_back(static_cast<char>(std::tolower(character)));
            continue;
        }
        if (!current.empty())
        {
            words.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) words.push_back(current);
    return words;
}

// Removes one leading phrase from the longest match in `phrases`, if any matches.
// Longest first so "find out about" is not shortened to "find out" and left holding a
// dangling "about". Returns whether anything was removed.
template <std::size_t Size>
bool StripLeading(
    std::string& value,
    const std::array<std::string_view, Size>& phrases)
{
    std::vector<std::string_view> ordered(phrases.begin(), phrases.end());
    std::sort(ordered.begin(), ordered.end(),
        [](const std::string_view left, const std::string_view right)
        {
            return left.size() > right.size();
        });
    const std::string lowered = Lower(value);
    for (const std::string_view phrase : ordered)
    {
        if (StartsWithPhrase(lowered, phrase))
        {
            value = Trim(value.substr(phrase.size()));
            return true;
        }
    }
    return false;
}

template <std::size_t Size>
bool StripTrailing(
    std::string& value,
    const std::array<std::string_view, Size>& phrases)
{
    std::vector<std::string_view> ordered(phrases.begin(), phrases.end());
    std::sort(ordered.begin(), ordered.end(),
        [](const std::string_view left, const std::string_view right)
        {
            return left.size() > right.size();
        });
    const std::string lowered = Lower(value);
    for (const std::string_view phrase : ordered)
    {
        if (EndsWithPhrase(lowered, phrase))
        {
            value = Trim(value.substr(0, lowered.size() - phrase.size()));
            return true;
        }
    }
    return false;
}
}

ResolvedLookupQuery ResolveLookupQuery(const std::string& input)
{
    ResolvedLookupQuery resolved;
    std::string body = TrimSentencePunctuation(Trim(input));
    if (body.empty())
    {
        resolved.reason = "The request was empty.";
        return resolved;
    }

    // Courtesy and command can alternate -- "please can you look up", "Revia, go and
    // search the web for" -- so both are stripped until neither matches rather than in
    // one fixed order. Bounded so a pathological input cannot spin here.
    bool removedVerb = false;
    for (int pass = 0; pass < 8; ++pass)
    {
        bool changed = StripLeading(body, LeadingCourtesies);
        if (StripLeading(body, LookupVerbs))
        {
            changed = true;
            removedVerb = true;
        }
        if (!changed) break;
        body = TrimSentencePunctuation(std::move(body));
    }

    // Filler only goes once a command was actually removed. Without that guard a bare
    // subject such as "the current Rider version" would lose its article for no reason,
    // and "if constexpr in C++" would lose the word the question is about.
    if (removedVerb)
    {
        for (int pass = 0; pass < 4; ++pass)
        {
            if (!StripLeading(body, LeadingFiller)) break;
        }
    }

    for (int pass = 0; pass < 4; ++pass)
    {
        if (!StripTrailing(body, TrailingCourtesies)) break;
        body = TrimSentencePunctuation(std::move(body));
    }
    body = TrimSentencePunctuation(std::move(body));

    if (body.empty())
    {
        resolved.reason = "The request named no subject to search for.";
        return resolved;
    }

    const std::string loweredBody = Lower(body);
    if (std::find(Delegations.begin(), Delegations.end(), loweredBody) !=
        Delegations.end())
    {
        // Choosing a subject Revia was not given is Curiosity's job, not this one.
        resolved.reason =
            "The request handed back the choice of subject instead of naming one.";
        return resolved;
    }

    const std::vector<std::string> words = Words(body);
    const auto contentWords = std::count_if(words.begin(), words.end(),
        [](const std::string& word)
        {
            return word.size() >= 2 && !IsStopword(word);
        });
    if (contentWords == 0)
    {
        resolved.reason = "The request contained no subject to search for.";
        return resolved;
    }

    resolved.resolved = true;
    resolved.query = body;
    resolved.reason = "Resolved the search subject from the request.";
    return resolved;
}

} // namespace revia::internet
