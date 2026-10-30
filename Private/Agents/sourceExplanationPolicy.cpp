#include "Agents/sourceExplanationPolicy.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <iterator>
#include <string>

namespace revia::agents
{
namespace
{
std::string NormalizeSourceRequest(const std::string_view input)
{
    std::string text;
    for (const unsigned char character : input)
    {
        if (std::isspace(character))
        {
            if (!text.empty() && text.back() != ' ')
                text += ' ';
        }
        else
            text += static_cast<char>(std::tolower(character));
    }
    const auto last = text.find_last_not_of(" .!?");
    return last == std::string::npos ? std::string{} : text.substr(0, last + 1);
}

bool IsSourceWordCharacter(const char character)
{
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
}

bool ContainsSourceWord(const std::string_view text, const std::string_view word)
{
    for (auto at = text.find(word); at != std::string_view::npos; at = text.find(word, at + 1))
        if ((at == 0 || !IsSourceWordCharacter(text[at - 1])) &&
            (at + word.size() == text.size() || !IsSourceWordCharacter(text[at + word.size()])))
            return true;
    return false;
}

bool ContainsSourceWord(const std::string_view text, const std::initializer_list<std::string_view> words)
{
    return std::any_of(words.begin(), words.end(), [text](const auto word) { return ContainsSourceWord(text, word); });
}

bool HasSourceSignals(const std::string_view text)
{
    if (ContainsSourceWord(
            text, {"code", "source", "function", "functions", "method", "methods", "expression", "expressions", "pointer", "pointers",
                      "iterator", "iterators", "vector", "array", "bounds", "index", "indices", "loop", "loops", "exception", "exceptions",
                      "throws", "nullptr", "dereference", "lifetime", "compiler", "compile", "debug", "debugging", "undefined behavior"}) ||
        text.find("c++") != std::string_view::npos || text.find("std::") != std::string_view::npos)
        return true;
    for (auto at = text.find('('); at != std::string_view::npos; at = text.find('(', at + 1))
        if (at > 0 && IsSourceWordCharacter(text[at - 1]))
            return true;
    for (const char quote : {'`', '\"', '\''})
    {
        for (auto begin = text.find(quote); begin != std::string_view::npos;)
        {
            const auto end = text.find(quote, begin + 1);
            if (end == std::string_view::npos)
                break;
            const auto expression = text.substr(begin + 1, end - begin - 1);
            if (!expression.empty() && (quote == '`' || expression.find_first_of("[](){};<>=*+/%") != std::string_view::npos))
                return true;
            begin = text.find(quote, end + 1);
        }
    }
    return false;
}

}

bool IsSourceExplanationRequest(const std::string_view input, const std::string_view previousAssistant)
{
    const auto text = NormalizeSourceRequest(input);
    if (ContainsSourceWord(text, {"story", "stories", "poem", "poems", "song", "songs", "joke", "jokes"}) &&
        ContainsSourceWord(text, {"tell", "write", "compose", "create", "invent", "give"}))
        return false;
    if (HasSourceSignals(text))
        return true;
    constexpr std::string_view followups[] = {"why", "how", "why not", "explain that", "can you explain that", "what do you mean",
        "what would you change", "what should i change", "how would you fix it", "how do i fix it", "what is the smallest correction",
        "what's the smallest correction", "show me the fix", "is that safe", "does that matter", "what happens then"};
    return std::find(std::begin(followups), std::end(followups), text) != std::end(followups) &&
           HasSourceSignals(NormalizeSourceRequest(previousAssistant));
}

}
