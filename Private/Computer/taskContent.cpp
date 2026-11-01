#include "Computer/taskContent.h"

#include "Planning/quotedText.h"
#include "Browser/browserTypes.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <string_view>
#include <vector>

namespace revia::computer
{

namespace
{


[[nodiscard]] std::string Lowered(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

// Quote handling is shared with the intent parser on purpose.
//
// Two copies of "what counts as a quotation" is how a payload comes to be read as a
// command by one parser and as content by the other -- which is exactly the defect this
// module now guards against. planning::FindQuoted is the single definition.
using revia::planning::QuotedSpan;

// Words that end a destination phrase rather than belonging to it.
//
// "to", "of" and "for" are deliberately absent, and the generalization matrix is what
// removed them. Character Map's field is called "Characters to copy"; stopping at "to"
// turned that into "characters", which matches nothing, and the run abstained on a field
// that was sitting right there with exactly the name the person had used. Interior
// prepositions are part of field names all over Windows -- "Search for", "Terms of
// service" -- and the marker that introduced the phrase has already been consumed by the
// time this is asked, so an interior "to" is not a second destination.
//
// What remains are the words that genuinely start a new clause: a location, a
// conjunction, or a word about the application rather than the field.
[[nodiscard]] bool EndsDestination(const std::string& word)
{
    static const std::array<std::string_view, 13> stops{
        "in", "into", "on", "at", "and", "then", "window",
        "windows", "app", "application", "program", "please", "using"};
    return std::find(stops.begin(), stops.end(), word) != stops.end();
}

// Words that describe the kind of thing a field is rather than naming it. "the Compose
// box" and "Compose" are the same destination said two ways, and an observer reports
// the second.
[[nodiscard]] bool IsFieldNoun(const std::string& word)
{
    static const std::array<std::string_view, 10> nouns{
        "box", "field", "line", "bar", "area", "input", "textbox", "control",
        "pane", "section"};
    return std::find(nouns.begin(), nouns.end(), word) != nouns.end();
}

[[nodiscard]] bool IsArticle(const std::string& word)
{
    static const std::array<std::string_view, 5> articles{"the", "a", "an", "my", "its"};
    return std::find(articles.begin(), articles.end(), word) != articles.end();
}

// The field the user named, from the text after the content.
//
// Loose on purpose. A user writes "the Compose box"; UI Automation reports "Compose".
// Anchoring the search after the quoted span is what stops "put 'meet me in the lobby'
// into Compose" resolving its destination to "lobby".
[[nodiscard]] std::string ParseDestination(const std::string& tail)
{
    const std::string lowered = Lowered(tail);
    std::size_t marked = std::string::npos;
    std::size_t at = std::string::npos;
    for (const std::string_view marker : {" into ", " in ", " to "})
    {
        const std::size_t found = lowered.find(marker);
        if (found == std::string::npos) continue;
        // The earliest marker wins, compared on where the marker starts rather than on
        // where its phrase does -- the markers differ in length, so comparing the
        // shifted positions would prefer the shorter word.
        if (marked == std::string::npos || found < marked)
        {
            marked = found;
            at = found + marker.size();
        }
    }
    if (at == std::string::npos) return {};

    std::istringstream words(lowered.substr(at));
    std::vector<std::string> phrase;
    std::string word;
    while (words >> word)
    {
        const bool endsPhrase = !word.empty() && std::string_view(",.;!?").find(word.back()) != std::string_view::npos;
        // Punctuation is not part of a field's name.
        while (!word.empty() && std::ispunct(static_cast<unsigned char>(word.back())) != 0)
        {
            word.pop_back();
        }
        if (word.empty()) continue;
        if (IsArticle(word) && phrase.empty()) continue;
        if (IsFieldNoun(word)) break;
        if (EndsDestination(word)) break;
        phrase.push_back(word);
        // A destination is a name, not a sentence. Three words is already generous for
        // something a person reads off a screen.
        if (endsPhrase || phrase.size() >= 3) break;
    }

    std::string destination;
    for (const std::string& part : phrase)
    {
        if (!destination.empty()) destination += ' ';
        destination += part;
    }
    return destination;
}

[[nodiscard]] std::string InferKind(const std::string& lowered)
{
    if (lowered.find("search") != std::string::npos) return "search";
    if (lowered.find("address") != std::string::npos ||
        lowered.find("url") != std::string::npos) return "address";
    if (lowered.find("file name") != std::string::npos ||
        lowered.find("filename") != std::string::npos) return "filename";
    if (lowered.find("message") != std::string::npos ||
        lowered.find("email") != std::string::npos ||
        lowered.find("note") != std::string::npos ||
        lowered.find("reply") != std::string::npos ||
        lowered.find("text ") != std::string::npos) return "message";
    return "text";
}

// Whether the person asked for the content to leave the machine, and what they called
// the control that would do it.
//
// Read conservatively and negation-first. "put this in the box, do not send it" contains
// the word "send", and a parser that only searched for verbs would turn a drafting
// request into a sent message -- which is the one mistake here that cannot be undone by
// a later step. So the negations are checked before the verbs, and anything unclear
// stays false.
struct SubmissionIntent
{
    bool requested = false;
    std::string verb;
};

[[nodiscard]] SubmissionIntent ReadSubmissionIntent(const std::string& lowered)
{
    static const std::array<std::string_view, 8> refusals{
        "do not send", "don't send", "dont send", "without sending", "do not submit",
        "don't submit", "do not post", "not to send"};
    if (std::any_of(refusals.begin(), refusals.end(), [&](const std::string_view phrase) {
            return lowered.find(phrase) != std::string::npos;
        }))
    {
        return {};
    }

    // Ordered, because the first one found is the word reported back and a person who
    // wrote "send" should see "send".
    static const std::array<std::string_view, 6> verbs{
        "send", "submit", "post", "publish", "reply", "share"};
    for (const std::string_view verb : verbs)
    {
        const std::size_t at = lowered.find(verb);
        if (at == std::string::npos) continue;
        // Whole word. "resend" and "respond" both contain a verb and neither is one,
        // and "sender" is a noun about a person.
        const bool startsWord =
            at == 0 || std::isalnum(static_cast<unsigned char>(lowered[at - 1])) == 0;
        const std::size_t after = at + verb.size();
        const bool endsWord = after >= lowered.size() ||
            std::isalnum(static_cast<unsigned char>(lowered[after])) == 0;
        if (startsWord && endsWord)
        {
            SubmissionIntent intent;
            intent.requested = true;
            intent.verb = std::string(verb);
            return intent;
        }
    }
    return {};
}

[[nodiscard]] bool AsksForDrafting(const std::string& lowered)
{
    static const std::array<std::string_view, 7> verbs{
        "compose ", "draft ", "write a ", "write me ", "write an ",
        "come up with ", "make up "};
    return std::any_of(verbs.begin(), verbs.end(), [&](const std::string_view verb) {
        return lowered.find(verb) != std::string::npos;
    });
}

// The same source-ordered spans carry both target names and exact content. Only
// unquoted words between spans can identify the placement verb or destination.
QuotedSpan ContentSpan(const std::string& request, const revia::planning::ParsedQuotation& parsed)
{
    std::size_t previous = 0;
    for (const auto& span : parsed.spans)
    {
        std::istringstream words(Lowered(request.substr(previous, span.opening - previous)));
        std::string word;
        while (words >> word)
        {
            if (word == "type" || word == "put" || word == "enter" || word == "write" ||
                word == "send" || word == "message" || word == "reply") return span;
        }
        previous = span.after;
    }
    return parsed.spans.empty() ? QuotedSpan{} : parsed.spans.front();
}

std::string QuotedDestination(const std::string& request, const revia::planning::ParsedQuotation& parsed, const QuotedSpan& content)
{
    std::size_t previous = 0;
    for (const auto& span : parsed.spans)
    {
        std::istringstream words(Lowered(request.substr(previous, span.opening - previous)));
        std::string word;
        std::string last;
        while (words >> word) last = word;
        previous = span.after;
        if (span.opening == content.opening) continue;
        // A later quoted application cannot replace an already named unquoted field.
        if (span.opening > content.after &&
            !ParseDestination(request.substr(content.after, span.opening - content.after)).empty()) continue;
        if (last == "in" || last == "into" || last == "to")
            return Lowered(request.substr(span.begin, span.end - span.begin));
    }
    return {};
}

struct AcceptanceToken
{
    std::string text;
    bool quoted = false;
    std::size_t opening = std::string::npos;
};

std::vector<AcceptanceToken> AcceptanceTokens(const std::string& request, const planning::ParsedQuotation& parsed)
{
    std::vector<AcceptanceToken> tokens;
    std::size_t at = 0;
    std::size_t quote = 0;
    while (at < request.size())
    {
        if (quote < parsed.spans.size() && at == parsed.spans[quote].opening)
        {
            const auto& span = parsed.spans[quote++];
            tokens.push_back({Lowered(request.substr(span.begin, span.end - span.begin)), true, span.opening});
            at = span.after;
        }
        else if (std::isspace(static_cast<unsigned char>(request[at])) != 0)
            ++at;
        else if (std::string_view(",.;!?").find(request[at]) != std::string_view::npos)
            tokens.push_back({std::string(1, request[at++])});
        else
        {
            const auto begin = at;
            const bool url = request.compare(at, 7, "http://") == 0 || request.compare(at, 8, "https://") == 0;
            while (at < request.size() && std::isspace(static_cast<unsigned char>(request[at])) == 0 &&
                (url || std::string_view(",.;!?").find(request[at]) == std::string_view::npos) &&
                (quote >= parsed.spans.size() || at != parsed.spans[quote].opening)) ++at;
            tokens.push_back({Lowered(request.substr(begin, at - begin))});
        }
    }
    return tokens;
}

bool SolePlacement(const std::string& request, const planning::ParsedQuotation& parsed, const TaskContent& content, std::string& application, std::string& browserOrigin)
{
    const auto tokens = AcceptanceTokens(request, parsed);
    std::size_t at = 0;
    const auto take = [&](const std::string_view word)
    {
        if (at == tokens.size() || tokens[at].quoted || tokens[at].text != word) return false;
        ++at;
        return true;
    };
    static_cast<void>(take("please"));
    if (take("can") || take("could") || take("would"))
    {
        if (!take("you")) return false;
        static_cast<void>(take("please"));
    }
    if (!(take("put") || take("type") || take("enter") || take("write") || take("paste"))) return false;
    if (take("the"))
    {
        if (!take("text")) return false;
    }
    else static_cast<void>(take("text"));
    const auto payload = ContentSpan(request, parsed);
    if (at == tokens.size() || !tokens[at].quoted || tokens[at++].opening != payload.opening) return false;
    if (!(take("in") || take("into") || take("to"))) return false;
    if (at < tokens.size() && !tokens[at].quoted && IsArticle(tokens[at].text)) ++at;
    // Multiword labels need explicit quoting; otherwise an unknown verb could be
    // swallowed as part of a destination. Unsupported wording asks for clarification.
    if (at == tokens.size() || tokens[at++].text != Lowered(content.destination)) return false;
    if (at < tokens.size() && !tokens[at].quoted && IsFieldNoun(tokens[at].text)) ++at;
    if (take("in") || take("on") || take("using") || take("at"))
    {
        static_cast<void>(take("the"));
        if (at == tokens.size()) return false;
        const auto& context = tokens[at++];
        if (context.text.empty()) return false;
        if (!context.quoted)
        {
            static const std::array<std::string_view, 16> instructions{
                "and", "then", "also", "after", "before", "open", "save", "check",
                "send", "submit", "post", "publish", "delete", "close", "run", "launch"};
            if (std::find(instructions.begin(), instructions.end(), context.text) != instructions.end())
                return false;
        }
        const bool applicationNoun = take("app") || take("application") || take("window");
        const bool explicitUrl = context.text.starts_with("https://") || context.text.starts_with("http://");
        if (!applicationNoun && !explicitUrl) return false;
        if (explicitUrl)
        {
            browserOrigin = browser::UrlOrigin(context.text);
            if (browserOrigin.empty()) return false;
        }
        if (applicationNoun && !explicitUrl)
        {
            if (!context.quoted && std::any_of(context.text.begin(), context.text.end(), [](const unsigned char character)
                { return std::isalnum(character) == 0 && character != '-' && character != '_'; })) return false;
            application = context.text;
        }
    }
    while (at < tokens.size())
    {
        if (take(",") || take(".") || take(";") || take("!") || take("?")) continue;
        if (take("please") || take("thanks")) continue;
        if (take("thank"))
        {
            if (!take("you")) return false;
            continue;
        }
        static_cast<void>(take("and"));
        if (take("do"))
        {
            if (!take("not")) return false;
        }
        else if (!(take("don't") || take("dont") || take("without"))) return false;
        if (!(take("send") || take("submit") || take("post") || take("sending"))) return false;
        if (!(take("it") || take("anything")))
        {
            if (take("the") && !(take("text") || take("message"))) return false;
        }
    }
    return true;
}

} // namespace

std::string ToString(const ContentProvenance value)
{
    switch (value)
    {
        case ContentProvenance::UserSupplied: return "user_supplied";
        case ContentProvenance::Drafted: return "drafted";
        case ContentProvenance::None:
        default: return "none";
    }
}

std::string ToString(const ContentRequirement value)
{
    switch (value)
    {
        case ContentRequirement::ExactUserContent: return "exact_user_content";
        case ContentRequirement::Drafted: return "drafted";
        case ContentRequirement::None:
        default: return "none";
    }
}

bool TaskAcceptanceContract::MatchesApplication(const std::string& executable) const
{
    if (application.empty()) return true;
    const auto canonical = [](std::string value)
    {
        value = Lowered(std::move(value));
        if (value.ends_with(".exe")) value.resize(value.size() - 4);
        return value;
    };
    return !executable.empty() && canonical(application) == canonical(executable);
}

TaskAcceptanceContract DeriveTaskAcceptance(const std::string& request)
{
    TaskAcceptanceContract contract;
    if (request.empty() || request.size() > MaximumTaskContentLength + 4096) return contract;
    const auto parsed = planning::ParseQuotation(request);
    if (!parsed.complete) return contract;
    const auto content = ExtractTaskContent(request);
    if (!content.RequiresExactContent() || content.value.empty() || content.destination.empty() || content.submissionRequested)
        return contract;
    contract.placementOnly = SolePlacement(request, parsed, content, contract.application, contract.browserOrigin);
    if (contract.placementOnly)
    {
        contract.surface = contract.browserOrigin.empty() ? TaskAcceptanceSurface::Native : TaskAcceptanceSurface::Browser;
        contract.detail = "The whole admitted request requires exact placement only.";
    }
    else contract.detail += " Remaining instructions (quoted data omitted): " + parsed.instruction.substr(0, 512);
    return contract;
}

TaskContent ExtractTaskContent(const std::string& request)
{
    TaskContent content;
    // What the request *says to do* is read with the quoted payload taken out; what it
    // says to *place* is read from the original. They are different questions and they
    // were being asked of the same string.
    //
    // 'type "hello then send it on messenger" into Compose' names a submission verb and
    // a platform, but only inside the quotation -- the user is dictating those words,
    // not issuing them. Reading the whole sentence set submissionRequested, which is
    // what tells the task progression a Send is authorised.
    const auto parsed = revia::planning::ParseQuotation(request);
    if (!parsed.complete) return content;
    const std::string instruction = Lowered(parsed.instruction);
    const SubmissionIntent submission = ReadSubmissionIntent(instruction);
    content.submissionRequested = submission.requested;
    content.submissionVerb = submission.verb;

    const QuotedSpan quoted = ContentSpan(request, parsed);
    if (quoted.found)
    {
        std::string value = request.substr(quoted.begin, quoted.end - quoted.begin);
        // Refused rather than truncated. Half a message placed into a box somebody then
        // sends is worse than a task that stops and says the content is too long.
        if (value.size() > MaximumTaskContentLength) return content;

        content.requirement = ContentRequirement::ExactUserContent;
        content.kind = InferKind(instruction);
        // Only what follows the quote. "put 'meet me in the lobby' into Compose" names
        // Compose, and a search over the whole request would name the lobby.
        content.destination = QuotedDestination(request, parsed, quoted);
        if (content.destination.empty())
            content.destination = ParseDestination(request.substr(quoted.after));
        // The same sentence with the content lifted out, for anything a model reads.
        // The quote marks stay where they were, so the description still reads as an
        // instruction about a quoted thing -- it simply no longer contains the thing.
        content.redacted = request.substr(0, quoted.begin) + PreparedContentToken +
            request.substr(quoted.end);
        content.value = std::move(value);
        return content;
    }

    if (AsksForDrafting(instruction))
    {
        content.requirement = ContentRequirement::Drafted;
        content.kind = InferKind(instruction);
        content.destination = ParseDestination(request);
        return content;
    }

    return content;
}

} // namespace revia::computer
