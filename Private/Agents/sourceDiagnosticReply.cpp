#include "Agents/sourceDiagnosticReply.h"
#include "Identity/promptMarkers.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <string>
#include <vector>

namespace revia::agents
{
namespace
{
constexpr std::size_t MaximumFieldBytes = 1024;
constexpr std::size_t MaximumReplyBytes = 2048;

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

std::string Lower(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string SafeTransport(std::string_view text)
{
    static const auto markers = []
    {
        std::vector<std::string> values;
        for (const auto marker : identity::markers::All)
            values.push_back(Lower(marker));
        values.push_back(Lower(identity::markers::RuntimeTurnContextEnd));
        values.emplace_back("[[silent]]");
        return values;
    }();
    const auto lowered = Lower(text);
    std::vector<bool> markerStarts(text.size(), false);
    for (const auto& marker : markers)
        for (auto at = lowered.find(marker); at != std::string::npos; at = lowered.find(marker, at + 1))
            markerStarts[at] = true;
    constexpr char hex[] = "0123456789ABCDEF";
    std::string safe;
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        const auto c = static_cast<unsigned char>(text[index]);
        if (c == '\n')
            safe += "\\n";
        else if (c == '\r')
            safe += "\\r";
        else if (c == '\t')
            safe += "\\t";
        else if (c < 0x20 || c == 0x7f || markerStarts[index])
        {
            safe += "\\x";
            safe += hex[c >> 4];
            safe += hex[c & 15];
        }
        else
            safe += static_cast<char>(c);
    }
    return safe;
}

std::string Code(std::string_view text)
{
    const auto safe = SafeTransport(text);
    std::size_t longest = 0, run = 0;
    for (const char c : safe)
    {
        run = c == '`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    const std::string fence(longest + 1, '`');
    const bool padding = !safe.empty() && (safe.front() == '`' || safe.back() == '`' || safe.front() == ' ' || safe.back() == ' ');
    return fence + (padding ? " " : "") + safe + (padding ? " " : "") + fence;
}

std::string Prose(std::string_view text)
{
    const auto safe = SafeTransport(Trim(text));
    std::string result;
    for (std::size_t index = 0; index < safe.size(); ++index)
    {
        if (safe[index] == '`')
        {
            const auto end = safe.find('`', index + 1);
            if (end != std::string::npos && end > index + 1)
            {
                result += Code(std::string_view(safe).substr(index + 1, end - index - 1));
                index = end;
            }
            else
                result += "\\`";
        }
        else if (safe[index] == '<')
            result += "&lt;";
        else if (safe[index] == '>')
            result += "&gt;";
        else
        {
            if (std::string_view("[]*_#!|").find(safe[index]) != std::string_view::npos)
                result += '\\';
            result += safe[index];
        }
    }
    if (!result.empty() && std::string_view(".!?").find(result.back()) == std::string_view::npos)
        result += '.';
    return result;
}

bool Complete(const SourceDiagnostic& diagnostic)
{
    for (const auto* field : {&diagnostic.function, &diagnostic.evidence, &diagnostic.condition, &diagnostic.consequence,
             &diagnostic.correction, &diagnostic.original, &diagnostic.replacement})
        if (Trim(*field).empty() || field->size() > MaximumFieldBytes)
            return false;
    return diagnostic.original != diagnostic.replacement;
}

std::string Correction(const SourceDiagnostic& diagnostic)
{
    auto correction = Prose(diagnostic.correction);
    const auto original = Code(diagnostic.original), replacement = Code(diagnostic.replacement);
    if (correction.find(original) == std::string::npos || correction.find(replacement) == std::string::npos)
        correction += " Exact change: " + original + " to " + replacement + '.';
    return correction;
}

std::string RequestText(std::string_view text)
{
    std::string result;
    for (const unsigned char c : Trim(text))
    {
        if (std::isspace(c))
        {
            if (!result.empty() && result.back() != ' ')
                result += ' ';
        }
        else
            result += static_cast<char>(std::tolower(c));
    }
    while (!result.empty() && std::string_view(" .!?").find(result.back()) != std::string_view::npos)
        result.pop_back();
    return result;
}

bool WordCharacter(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool HasWord(std::string_view text, std::string_view word)
{
    if (word.empty())
        return false;
    for (auto at = text.find(word); at != std::string_view::npos; at = text.find(word, at + 1))
        if ((at == 0 || !WordCharacter(text[at - 1])) && (at + word.size() == text.size() || !WordCharacter(text[at + word.size()])))
            return true;
    return false;
}

bool HasAnyWord(std::string_view text, std::initializer_list<std::string_view> words)
{
    return std::any_of(words.begin(), words.end(), [text](auto word) { return HasWord(text, word); });
}

std::string_view WithoutPolitePrefix(std::string_view text)
{
    for (int count = 0; count < 3; ++count)
    {
        bool removed = false;
        for (const auto prefix : {"please ", "can you ", "could you ", "would you "})
            if (text.starts_with(prefix))
            {
                text.remove_prefix(std::string_view(prefix).size());
                removed = true;
                break;
            }
        if (!removed)
            break;
    }
    return text;
}

bool StartsAction(std::string_view text)
{
    text = WithoutPolitePrefix(Trim(text));
    for (const auto action :
        {"fix ", "edit ", "apply ", "patch ", "run ", "execute ", "build ", "test ", "save ", "delete ", "remove ", "install ", "launch ",
            "open ", "compile ", "commit ", "push ", "deploy ", "modify ", "update ", "change ", "replace ", "correct "})
        if (text.starts_with(action))
            return true;
    return false;
}

bool ActionRequested(std::string_view text)
{
    if (StartsAction(text) || HasAnyWord(text, {"edit", "apply", "patch", "run", "execute", "build", "test", "save", "delete", "install",
                                                   "launch", "compile", "commit", "push", "deploy"}))
        return true;
    for (const auto separator : {" and ", " then ", " also ", ", ", "; ", ". ", "? ", "! ", ": "})
        for (auto at = text.find(separator); at != std::string_view::npos; at = text.find(separator, at + 1))
            if (StartsAction(text.substr(at + std::string_view(separator).size())))
                return true;
    return false;
}

bool ExcludedReplyRequest(std::string_view text)
{
    return HasAnyWord(
               text, {"story", "stories", "poem", "poems", "song", "songs", "joke", "jokes", "haiku", "fiction", "json", "xml", "yaml",
                         "csv", "markdown", "schema", "format", "table", "translate", "translation", "repeat", "quote", "reply", "respond",
                         "output", "write", "say", "language", "bullet", "fence", "only", "exactly", "ignore", "pretend", "roleplay"}) ||
           text.find("```") != std::string_view::npos;
}
}

std::string FormatSourceDiagnostic(const SourceDiagnostic& diagnostic, const bool correctionOnly)
{
    if (!Complete(diagnostic))
        return {};
    const auto function = Code(diagnostic.function);
    const auto reply = correctionOnly ? "In " + function + ": " + Correction(diagnostic)
                                      : "In " + function + " (" + Code(diagnostic.evidence) + "): " + Prose(diagnostic.condition) + ' ' +
                                            Prose(diagnostic.consequence) + ' ' + Correction(diagnostic);
    return reply.size() <= MaximumReplyBytes ? reply : std::string{};
}

bool WantsSourceDiagnosticCorrectionOnly(const std::string_view input)
{
    if (input.size() > 1024)
        return false;
    const auto normalized = RequestText(input);
    const auto request = WithoutPolitePrefix(normalized);
    if (ExcludedReplyRequest(request) || ActionRequested(request))
        return false;
    const bool minimal = HasAnyWord(request, {"smallest", "minimal"}) && HasAnyWord(request, {"fix", "correction"});
    return minimal || request == "show me the fix" || request == "show the fix" || request == "show me the correction" ||
           request == "show the correction" || request == "which function" || request.starts_with("which function ");
}

bool IsSourceDiagnosticFollowup(const std::string_view input, const std::string_view previousAssistant, const SourceDiagnostic& diagnostic)
{
    if (input.size() > 1024 || !Complete(diagnostic))
        return false;
    const auto normalized = RequestText(input);
    if (ExcludedReplyRequest(normalized))
        return false;
    const auto request = WithoutPolitePrefix(normalized);
    if (request.empty() || ActionRequested(request))
        return false;
    constexpr std::array shortFollowups = {"why", "why not", "how", "explain that", "explain the fix", "explain that finding",
        "what do you mean", "which function", "which function is that", "which function are you referring to", "what function",
        "what would you change", "what should i change", "how would you fix it", "how do i fix it", "what is the smallest correction",
        "what's the smallest correction", "what is the minimal fix", "what's the minimal fix", "show me the fix", "show the fix",
        "show me the correction", "show the correction",
        "which function in that saved file are you referring to, and what is the smallest correction"};
    const auto previous = Trim(previousAssistant);
    const auto observation = FormatSourceDiagnostic(diagnostic), correction = FormatSourceDiagnostic(diagnostic, true);
    const bool matchingPrevious =
        !previous.empty() && ((!observation.empty() && previous == observation) || (!correction.empty() && previous == correction));
    if (matchingPrevious && std::find(shortFollowups.begin(), shortFollowups.end(), request) != shortFollowups.end())
        return true;
    constexpr std::array currentSourceQuestions = {"explain the bounds issue in the current saved source",
        "explain the lifetime issue in the current saved source", "explain the issue in the current saved source",
        "what is the smallest correction to the current saved source", "what is the minimal fix for the current saved source"};
    if (std::find(currentSourceQuestions.begin(), currentSourceQuestions.end(), request) != currentSourceQuestions.end())
        return true;
    struct Question
    {
        std::string_view prefix;
        std::string_view suffix;
    };
    constexpr Question questions[] = {{"why does ", " fail"}, {"why is ", " failing"}, {"what is wrong with ", ""},
        {"what's wrong with ", ""}, {"explain ", ""}, {"explain the issue in ", ""}, {"explain the defect in ", ""},
        {"explain the bounds issue in ", ""}, {"explain the lifetime issue in ", ""}, {"what is the smallest correction to ", ""},
        {"what's the smallest correction to ", ""}, {"what is the minimal fix for ", ""}, {"what's the minimal fix for ", ""},
        {"how would you fix ", ""}, {"how do i fix ", ""}, {"what should i change in ", ""}, {"what would you change in ", ""}};
    const auto function = Lower(diagnostic.function);
    for (const auto& name : {function, '`' + function + '`'})
        for (const auto& question : questions)
            if (request == std::string(question.prefix) + name + std::string(question.suffix))
                return true;
    return false;
}
}
