#include "Perception/savedFileObservation.h"
#include "Audit/contentDigest.h"
#include "Core/utf8.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace revia::perception
{
namespace
{
bool IdentifierByte(const unsigned char c)
{
    return std::isalnum(c) || c == '_' || c >= 0x80;
}

bool RawStringPrefix(const std::string& source, const std::size_t index)
{
    auto begin = index;
    while (begin > 0 && IdentifierByte(static_cast<unsigned char>(source[begin - 1])))
    {
        --begin;
    }
    const auto prefix = source.substr(begin, index - begin + 1);
    return prefix == "R" || prefix == "u8R" || prefix == "uR" || prefix == "UR" || prefix == "LR";
}

bool ConsumeRawString(const std::string& source, std::size_t& index)
{
    const auto delimiterBegin = index + 2;
    auto opening = delimiterBegin;
    for (; opening < source.size() && source[opening] != '('; ++opening)
    {
        const auto c = static_cast<unsigned char>(source[opening]);
        if (opening - delimiterBegin >= 16 || c <= ' ' || c > '~' || c == ')' || c == '\\')
        {
            return false;
        }
    }
    if (opening == source.size())
    {
        return false;
    }
    const auto terminator = ')' + source.substr(delimiterBegin, opening - delimiterBegin) + '"';
    const auto closing = source.find(terminator, opening + 1);
    if (closing == std::string::npos)
    {
        return false;
    }
    index = closing + terminator.size() - 1;
    return true;
}

void ConsumeNumber(const std::string& source, std::size_t& index)
{
    while (index + 1 < source.size())
    {
        const auto next = static_cast<unsigned char>(source[index + 1]);
        const bool separator = next == '\'' && index + 2 < source.size() &&
                               (std::isalnum(static_cast<unsigned char>(source[index + 2])) || source[index + 2] == '_');
        const bool exponentSign =
            (next == '+' || next == '-') && (source[index] == 'e' || source[index] == 'E' || source[index] == 'p' || source[index] == 'P');
        if (!std::isalnum(next) && next != '_' && next != '.' && !separator && !exponentSign)
        {
            break;
        }
        ++index;
    }
}

bool CompleteFragment(const std::string& source)
{
    std::vector<char> stack;
    char quote = 0;
    bool escaped = false, lineComment = false, blockComment = false;
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        const char c = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : 0;
        if (lineComment) { if (c == '\n') lineComment = false; continue; }
        if (blockComment) { if (c == '*' && next == '/') { blockComment = false; ++i; } continue; }
        if (quote)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '/' && next == '/') { lineComment = true; ++i; continue; }
        if (c == '/' && next == '*') { blockComment = true; ++i; continue; }
        if (c == 'R' && next == '"' && RawStringPrefix(source, i))
        {
            if (!ConsumeRawString(source, i)) return false;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) &&
            (i == 0 || !IdentifierByte(static_cast<unsigned char>(source[i - 1]))))
        {
            ConsumeNumber(source, i);
            continue;
        }
        if (c == '\'' || c == '"')
        {
            quote = c;
            continue;
        }
        if (c == '{' || c == '(' || c == '[')
            stack.push_back(c);
        else if (c == '}' || c == ')' || c == ']')
        {
            if (stack.empty())
                return false;
            const char expected = c == '}' ? '{' : c == ')' ? '(' : '[';
            if (stack.back() != expected)
                return false;
            stack.pop_back();
        }
    }
    return stack.empty() && !quote && !blockComment;
}
}

bool ValidateSavedFileObservation(const SavedFileObservation& source,
    const std::chrono::system_clock::time_point now, std::string& reason)
{
    using namespace std::chrono_literals;
    const auto reject = [&](const char* value) { reason = value; return false; };
    if (source.occurredAt > now + 5s || now - source.occurredAt > 120s)
        return reject("The saved-file event is stale or has a future timestamp.");
    if (!source.projectRoot.is_absolute() || !source.path.is_absolute())
        return reject("Saved-file events require absolute approved project and file paths.");
    const auto relative = source.path.lexically_normal().lexically_relative(source.projectRoot.lexically_normal());
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        return reject("The file is outside the declared project.");
    std::string extension = source.path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    constexpr const char* supported[] = {".c", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".hxx"};
    if (std::find(std::begin(supported), std::end(supported), extension) == std::end(supported))
        return reject("This first saved-file integration accepts C and C++ source only.");
    if (source.contents.empty() || source.contents.size() > 16384 || source.contents.find('\0') != std::string::npos ||
        !utf8::IsValid(source.contents))
        return reject("The source snapshot must be nonempty, bounded UTF-8 text.");
    if (source.revision != audit::ContentDigest(source.contents))
        return reject("The saved file no longer matches the reported revision.");
    for (const char* sensitive : {"-----BEGIN PRIVATE KEY-----", "-----BEGIN RSA PRIVATE KEY-----", "sk-proj-", "ghp_"})
        if (source.contents.find(sensitive) != std::string::npos)
            return reject("The source resembles credential material; optional observation is suppressed.");
    // A conservative nomination filter, not a parser or a diagnostic. Incomplete
    // saves are left alone instead of prompting a critique of unfinished typing.
    if (!CompleteFragment(source.contents))
        return reject("The saved fragment appears incomplete; no review was nominated.");
    reason.clear();
    return true;
}

std::string SavedFileContext(const SavedFileObservation& source)
{
    std::ostringstream out;
    out << SavedFileContextPrefix
        << "Source: saved_file; author: unknown; no unsaved editor or screen access.\n"
        << "Project: " << source.projectRoot.generic_string() << "\nFile: " << source.path.generic_string()
        << "\nSHA256 revision: " << source.revision << "\nObserved UTC epoch milliseconds: "
        << std::chrono::duration_cast<std::chrono::milliseconds>(source.occurredAt.time_since_epoch()).count()
        << "\nNo build/test has run for this observation. Only this file was read; callers and other types may require inspection.\n"
        << "This is the stated saved snapshot, not proof that a later edit did not happen. An approved patch requires a fresh read.\n"
        << SavedSourceMarker
        << utf8::Prefix(source.contents, 6000);
    return out.str();
}
}
