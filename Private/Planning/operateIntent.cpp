#include "Planning/operateIntent.h"
#include "Planning/quotedText.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <string_view>

namespace revia::planning
{

namespace
{

std::string Normalize(const std::string& value)
{
    std::string lowered;
    lowered.reserve(value.size());
    bool pendingSpace = false;
    for (const unsigned char character : value)
    {
        if (std::isspace(character) != 0)
        {
            pendingSpace = !lowered.empty();
            continue;
        }
        // Punctuation that only ever ends a sentence is dropped so "open edge." and
        // "open edge" are the same request. Everything else is kept, because a dot
        // inside facebook.com is part of the target.
        if (character == '?' || character == '!' || character == ',')
        {
            continue;
        }
        if (pendingSpace) lowered.push_back(' ');
        pendingSpace = false;
        lowered.push_back(static_cast<char>(std::tolower(character)));
    }
    while (!lowered.empty() && lowered.back() == '.') lowered.pop_back();
    return lowered;
}

bool StartsWith(const std::string& value, const std::string_view prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

// Speech stacks openers: "okay so hey revia can you please go ahead and open edge".
constexpr int MaximumCourtesyPasses = 6;

// Politeness and address, removed so the verb can be tested at the front. "Can you open
// Edge" is a request, not a question about her abilities, and speech produces far more
// of these than typing does.
bool StripLeadingCourtesy(std::string& value)
{
    // "let's" is the one that sent this list back for revision. "lets pull up
    // facebook" is how a person actually asks, and because the verb has to sit at the
    // front, the bare "lets" left it unmatched -- the sentence never reached the action
    // path at all, and the conversation model answered as though it had. It is a
    // suggestion-to-act form, exactly like "can you", not a report of anything.
    //
    // The second group is how a spoken request actually starts: "okay", "alright so",
    // "and then", "I need you to". None of them names an action, so removing one can only
    // expose a verb that was already there; "okay" alone, or "so it opens slowly", still
    // matches nothing. A longer form is listed before any shorter one it begins with.
    static constexpr std::string_view prefixes[] = {
        "hey revia ", "revia ", "please ", "could you please ", "can you please ",
        "would you please ", "will you please ",
        "could you ", "can you ", "would you ", "will you ", "go ahead and ",
        "i want you to ", "i need you to ", "i would like you to ", "i'd like you to ", "you can ",
        "now ", "just ", "let's ", "lets ", "let us ", "how about you ",
        "what about you ",
        "okay ", "ok ", "alright ", "all right ", "hey ", "so ", "and then ", "and ", "then ", "also "};
    for (const std::string_view prefix : prefixes)
    {
        if (StartsWith(value, prefix))
        {
            value.erase(0, prefix.size());
            return true;
        }
    }
    return false;
}

// Verbs that name operating this machine. Multi-word forms come first so "go to" is not
// matched as the bare "go".
// "open up" is deliberately absent. As a phrasal verb it is almost always figurative --
// open up to me, open up about it -- and listing it consumed the "up" before the
// figurative check could see it, so "open up to me" arrived as a request to operate.
// Left to the bare "open", the remainder is "up to me" and the guard below catches it,
// while "open up edge" still matches on "open".
const std::array<std::string_view, 30>& OperateVerbs()
{
    static const std::array<std::string_view, 30> verbs = {
        "go to ", "navigate to ", "browse to ", "pull up ", "bring up ", "switch to ", "double-click ",
        "double click ", "right-click ", "right click ", "log in to ", "sign in to ",
        "open ", "launch ", "start ", "run ", "close ", "quit ", "click ", "type ",
        "press ", "scroll ", "minimize ", "maximize ", "minimise ", "maximise ", "focus ", "select ",
        "refresh ", "reload "};
    return verbs;
}

// Whether what follows the verb is `phrase`, or begins with it as whole words. A phrase
// written with a trailing space is a plain prefix: "with " covers "with a joke".
bool Begins(const std::string& remainder, const std::string_view phrase)
{
    if (!phrase.empty() && phrase.back() == ' ')
        return StartsWith(remainder, phrase);
    return StartsWith(remainder, phrase) && (remainder.size() == phrase.size() || remainder[phrase.size()] == ' ');
}

bool BeginsAny(const std::string& remainder, const std::initializer_list<std::string_view> phrases)
{
    return std::any_of(phrases.begin(), phrases.end(), [&](const std::string_view phrase) { return Begins(remainder, phrase); });
}

// "start telling me", "quit joking": an -ing word straight after the verb says what
// somebody is doing, not which program.
bool BeginsWithGerund(const std::string& remainder)
{
    const std::string_view word = std::string_view(remainder).substr(0, remainder.find(' '));
    return word.size() >= 5 && word.ends_with("ing");
}

// Figurative uses of the same verbs. Without these "open up to me" and "start a
// conversation" would both reach for the mouse, which is the failure that would make a
// natural-language route worse than no route at all.
//
// The cost of a miss here is paid in ordinary conversation, and with pointer and
// keyboard control off it is paid every time: "go to sleep", "focus on me" and "quit
// joking" each matched, and each was answered with the line about turning control on
// in Permissions instead of a reply. So the common conversational objects of each verb
// are named; a request that names a program, a window or a page is not among them.
bool ObjectIsFigurative(const std::string_view verb, const std::string& remainder)
{
    // The listener or the speaker as the object: "run me through it", "focus yourself".
    if (BeginsAny(remainder, {"me", "us", "yourself", "myself", "ourselves"}))
        return true;
    // A person's own parts, for any verb: "open your mind", "close your eyes".
    if (BeginsAny(remainder, {"your mind", "your heart", "your eyes", "your mouth", "your ears", "my eyes", "my mind", "my heart"}))
        return true;
    if (BeginsAny(remainder, {"up to me", "up to you", "up about", "up more", "a conversation", "the conversation", "a discussion",
            "a dialogue", "a chat", "a debate", "an argument", "over", "again from the top", "fresh", "thinking"}))
        return true;

    const auto is = [verb](const std::string_view name) { return verb == name; };
    if (is("go to "))
        return BeginsAny(remainder, {"sleep", "bed", "hell", "work", "school", "class", "lunch", "dinner", "town", "the store",
            "the shop", "the gym", "the bathroom", "the next question", "the next topic"});
    if (is("start "))
        return BeginsWithGerund(remainder) || BeginsAny(remainder, {"with ", "by ", "from ", "without ", "off", "now", "small",
            "simple", "slow", "slowly", "a story", "a poem", "a song", "a new topic", "a new conversation"});
    if (is("quit "))
        return BeginsWithGerund(remainder) || BeginsAny(remainder, {"it", "that", "this", "being ", "your ", "while "});
    if (is("run "))
        return BeginsAny(remainder, {"that by ", "it by ", "this by ", "with ", "through ", "into ", "on ", "over ", "by ", "for ",
            "away", "out", "more", "faster", "the numbers", "the risk"});
    if (is("close "))
        return BeginsAny(remainder, {"the topic", "the subject", "the discussion", "the matter", "the case", "the deal", "the gap",
            "the chapter", "that chapter", "the loop", "to ", "enough"});
    if (is("open "))
        return remainder == "up" || BeginsAny(remainder, {"with ", "by "});
    if (is("focus "))
    {
        // "focus on me", "focus on what I said". A window named after "on" is still a request.
        if (StartsWith(remainder, "on "))
            return BeginsAny(remainder.substr(3), {"me", "you", "us", "him", "her", "them", "it", "this", "that", "what", "how", "why",
                "being", "getting", "doing", "your ", "my ", "our ", "the task", "the question", "the problem", "the point",
                "the conversation", "the present", "the positive", "the bright side", "something", "one thing"});
        return BeginsAny(remainder, {"more", "please", "harder", "better", "up"});
    }
    if (is("press "))
        return remainder == "on" || BeginsAny(remainder, {"on with ", "ahead", "forward", "the issue", "the point", "the matter",
            "charges", "your luck"});
    if (is("select "))
        return BeginsAny(remainder, {"a topic", "whatever", "whichever"});
    if (is("scroll "))
        return BeginsAny(remainder, {"through your "});
    if (is("launch "))
        return BeginsAny(remainder, {"into "});
    if (is("pull up "))
        return BeginsAny(remainder, {"a chair", "a seat", "a stool"});
    if (is("bring up "))
        return BeginsAny(remainder, {"that topic", "the topic", "the subject", "the past", "the issue", "that again", "it again",
            "politics", "religion", "old "});
    if (is("switch to "))
        return BeginsAny(remainder, {"another topic", "a different topic", "a new topic", "another subject", "a different subject",
            "something else", "english", "spanish", "french", "german", "italian", "portuguese", "japanese", "chinese", "korean",
            "another voice", "a different voice", "plan b"});
    if (is("minimize ") || is("maximize ") || is("minimise ") || is("maximise "))
        return BeginsAny(remainder, {"the risk", "risk", "the damage", "the chance", "the chances", "the impact", "profit", "profits",
            "your ", "our "});
    if (is("type "))
        return BeginsAny(remainder, {"of "});
    if (is("refresh ") || is("reload "))
        return BeginsAny(remainder, {"my memory", "your memory"});
    return false;
}

// A statement about what already happened is not an instruction. Checked after courtesy
// stripping so "i want you to open edge" survives and "i opened edge" does not.
bool IsReport(const std::string& value)
{
    static constexpr std::array<std::string_view, 8> reports = {
        "i ", "we ", "you already ", "you just ", "you were ", "you have ", "it ",
        "that "};
    for (const std::string_view prefix : reports)
    {
        if (StartsWith(value, prefix)) return true;
    }
    return false;
}

} // namespace

bool RequestsExternalMessage(const std::string& input)
{
    // Read with the quoted payload removed.
    //
    // 'type "hello then send it on messenger" into Compose' places text in a field. It
    // is not a request for messaging authority -- but "messenger" and "then send" are
    // both inside the quotation, and reading the whole sentence found them, classified
    // the turn as an external message, scheduled a Send and, under OwnerFullAccess,
    // skipped the confirmation that would have shown the user what was about to leave
    // the machine. Words a person is dictating are content, not commands.
    std::string value = Normalize(WithoutQuotedPayload(input));
    for (int pass = 0; pass < MaximumCourtesyPasses && StripLeadingCourtesy(value); ++pass) {}
    if (value.empty() || IsReport(value)) return false;
    const bool platform = value.find("facebook") != std::string::npos ||
        value.find("messenger") != std::string::npos ||
        value.find("discord") != std::string::npos ||
        value.find("email") != std::string::npos;
    const auto messageClause = [](const std::string& clause)
    {
        return StartsWith(clause, "message ") || StartsWith(clause, "send ") ||
            StartsWith(clause, "reply to ") || StartsWith(clause, "post ");
    };
    bool destination = false;
    for (const std::string_view service : {"facebook", "messenger", "discord", "email"})
        for (const std::string_view preposition : {" on ", " via ", " through ", " using "})
            destination = destination ||
                value.find(std::string(preposition) + std::string(service)) != std::string::npos;
    if (destination && messageClause(value)) return true;
    // A compound desktop request may explicitly include a message after navigation.
    // Only an imperative clause grants that scope; quoted discussion and drafts do not.
    for (const std::string_view separator : {" and ", " then "})
    {
        const auto at = value.find(separator);
        if (at != std::string::npos && platform &&
            messageClause(value.substr(at + separator.size())))
            return DetectOperateRequest(value.substr(0, at)).matched;
    }
    return false;
}

OperateIntent DetectOperateRequest(const std::string& input)
{
    OperateIntent intent;
    // Same rule, same reason: what a request authorises is read from the instruction,
    // never from the payload. ExtractTaskContent reads the original, because placing
    // the words is the one job that needs them.
    std::string value = Normalize(WithoutQuotedPayload(input));
    if (value.empty() || value.front() == '/') return intent;

    // Repeated because speech stacks them: "hey revia can you please open edge".
    for (int pass = 0; pass < MaximumCourtesyPasses && StripLeadingCourtesy(value); ++pass) {}
    if (value.empty() || IsReport(value)) return intent;

    if (RequestsExternalMessage(value))
    {
        intent.matched = true;
        intent.verb = "message";
        return intent;
    }

    for (const std::string_view verb : OperateVerbs())
    {
        if (!StartsWith(value, verb)) continue;
        const std::string remainder = value.substr(verb.size());
        // A verb with nothing after it names no target, so there is nothing to do.
        if (remainder.empty() || ObjectIsFigurative(verb, remainder)) return intent;
        intent.matched = true;
        intent.verb = std::string(verb.substr(0, verb.size() - 1));
        return intent;
    }
    return intent;
}

} // namespace revia::planning
