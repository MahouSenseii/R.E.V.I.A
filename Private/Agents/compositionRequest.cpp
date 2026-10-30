#include "Agents/compositionRequest.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <vector>

namespace revia::agents
{
namespace
{
bool OneOf(const std::string_view word, const std::initializer_list<std::string_view> choices)
{
    return std::find(choices.begin(), choices.end(), word) != choices.end();
}

bool CompositionVerb(const std::string_view word)
{
    return OneOf(word, {"tell", "write", "compose", "invent", "create", "make", "give"});
}

bool CreativeForm(const std::string_view word)
{
    return OneOf(word, {"story", "stories", "poem", "poems", "haiku", "joke", "jokes", "scene", "scenes", "fable", "fables", "limerick",
                           "verse", "dialogue"});
}

std::vector<std::string_view> Words(const std::string_view text)
{
    std::vector<std::string_view> words;
    for (std::size_t at = 0; at < text.size();)
    {
        if (!std::isalnum(static_cast<unsigned char>(text[at])) && text[at] != '_')
        {
            ++at;
            continue;
        }
        const auto begin = at++;
        while (at < text.size() && (std::isalnum(static_cast<unsigned char>(text[at])) || text[at] == '_'))
            ++at;
        words.emplace_back(text.data() + begin, at - begin);
    }
    return words;
}

std::string_view TrimSpaces(std::string_view text)
{
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    return text;
}

bool LengthFormat(const std::string_view text)
{
    if (text.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789 ") != std::string_view::npos)
        return false;
    const auto words = Words(text);
    return words.size() == 3 && words[0] == "in" &&
           OneOf(words[1], {"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "1", "2", "3", "4", "5", "6",
                               "7", "8", "9", "10"}) &&
           OneOf(words[2], {"sentence", "sentences", "line", "lines", "word", "words"});
}

bool SimpleSubjectShape(const std::vector<std::string_view>& words)
{
    for (const auto word : words)
        if (OneOf(word, {"as", "to", "into", "with", "for", "by", "via", "in", "on", "at", "or", "after", "when", "while", "until",
                            "because", "if", "where", "who", "which", "whose", "how", "why", "containing"}))
            return false;
    std::size_t begin = 0;
    for (std::size_t at = 0; at <= words.size(); ++at)
    {
        if (at < words.size() && words[at] != "and")
            continue;
        const auto count = at - begin;
        if (count == 0)
            return false;
        const bool article = OneOf(words[begin], {"a", "an", "the"});
        if (article ? (count < 2 || count > 3) : count > (begin == 0 ? 2U : 1U))
            return false;
        begin = at + 1;
    }
    return true;
}

bool ClosedCompositionTail(std::string_view tail)
{
    tail = TrimSpaces(tail);
    const auto comma = tail.find(',');
    if (comma != std::string_view::npos)
    {
        if (!LengthFormat(TrimSpaces(tail.substr(comma + 1))))
            return false;
        tail = TrimSpaces(tail.substr(0, comma));
    }
    if (tail.empty())
        return true;
    if (!tail.starts_with("about "))
        return false;
    return SimpleSubjectShape(Words(tail.substr(6)));
}

bool DependencyOrAdditionalTask(const std::string_view word)
{
    return OneOf(word, {"my", "our", "your", "this", "that", "these", "those", "it", "its", "them", "their", "again", "another", "same",
                           "above", "below", "before", "previous", "earlier", "remember", "recall", "history", "conversation", "chat",
                           "using", "use", "based", "from", "attached", "attachment", "document", "file", "files", "folder", "directory",
                           "project", "repository", "saved", "code", "source", "screen", "screenshot", "image", "images", "photo", "photos",
                           "picture", "pictures", "audio", "video", "videos", "music", "recording", "song", "sing", "voice", "latest",
                           "newest", "current", "currently", "recent", "results", "factual", "statistics", "actual", "facts", "citation",
                           "citations", "references", "according", "today", "yesterday", "tomorrow", "news", "weather", "forecast", "price",
                           "score", "schedule", "agent", "agents", "worker", "workers", "delegate", "also", "then", "afterward"}) ||
           OneOf(word, {"read", "open", "close", "launch", "click", "type", "press", "navigate", "start", "stop", "save", "send", "email",
                           "text", "post", "publish", "print", "upload", "download", "remind", "set", "cancel", "delete", "modify", "edit",
                           "update", "replace", "change", "execute", "run", "calculate", "search", "browse", "look", "check", "verify",
                           "fetch", "find", "list", "access", "inspect", "research", "copy", "paste", "share", "install", "buy", "purchase",
                           "book", "order", "reserve", "call", "generate", "explain", "answer", "summarize", "translate", "compare",
                           "analyze", "solve", "count", "include", "do"});
}
}

bool IsSelfContainedCompositionRequest(const std::string_view userAuthoredInput)
{
    if (userAuthoredInput.empty() || userAuthoredInput.size() > 1024)
        return false;
    std::string text;
    for (const unsigned char character : userAuthoredInput)
    {
        if (character == ' ' || character == '\t')
        {
            if (!text.empty() && text.back() != ' ')
                text += ' ';
        }
        else
        {
            // Paths, markup, quoted requests and additional sentences keep the ordinary tool path.
            if (character < 32 || character >= 127 ||
                (!std::isalnum(character) && std::string_view("_'-+,.!?").find(character) == std::string_view::npos))
                return false;
            text += static_cast<char>(std::tolower(character));
        }
    }
    const auto end = text.find_last_not_of(" .!?");
    if (end == std::string::npos)
        return false;
    text.resize(end + 1);
    if (text.find_first_of(".!?") != std::string::npos)
        return false;
    for (auto quote = text.find('\''); quote != std::string::npos; quote = text.find('\'', quote + 1))
        if (quote == 0 || quote + 1 == text.size() || !std::isalnum(static_cast<unsigned char>(text[quote - 1])) ||
            !std::isalnum(static_cast<unsigned char>(text[quote + 1])))
            return false;

    const auto words = Words(text);
    std::size_t verb = !words.empty() && words.front() == "please" ? 1 : 0;
    if (verb >= words.size() || !CompositionVerb(words[verb]))
        return false;
    std::size_t at = verb + 1;
    if (words[verb] == "make" && (at == words.size() || words[at++] != "up"))
        return false;
    if (at < words.size() && words[at] == "me")
        ++at;
    while (at < words.size() &&
           OneOf(words[at], {"a", "an", "the", "some", "short", "brief", "little", "tiny", "original", "imaginary", "fictional", "funny",
                                "playful", "whimsical", "bedtime", "silly", "witty", "cheerful", "dark", "spooky", "one", "two", "three",
                                "four", "five", "six", "line", "lines", "sentence", "sentences"}))
        ++at;
    if (at == words.size() || !CreativeForm(words[at]))
        return false;
    const auto form = at;
    for (std::size_t index = 0; index < words.size(); ++index)
        if (DependencyOrAdditionalTask(words[index]) || (index != verb && CompositionVerb(words[index])) ||
            (index != form && CreativeForm(words[index])))
            return false;
    // Only a bounded subject shape and an optional length suffix qualify; extra clauses retain tools.
    const auto tailBegin = static_cast<std::size_t>(words[form].data() + words[form].size() - text.data());
    return ClosedCompositionTail(std::string_view(text).substr(tailBegin));
}
}
