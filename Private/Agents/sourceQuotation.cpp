#include "Agents/sourceQuotation.h"

#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <vector>

namespace revia::agents
{
namespace
{
struct ByteSpan
{
    std::size_t begin;
    std::size_t end;
    bool foldableWhitespace = false;
};

struct NormalizedSource
{
    std::string text;
    std::vector<ByteSpan> spans;

    void AppendExact(const std::string_view source, const std::size_t begin, const std::size_t end)
    {
        for (auto index = begin; index < end; ++index)
        {
            text += source[index];
            spans.push_back({index, index + 1, false});
        }
    }

    void AppendWhitespace(const std::size_t begin, const std::size_t end)
    {
        if (!spans.empty() && spans.back().foldableWhitespace && spans.back().end == begin)
            spans.back().end = end;
        else
        {
            text += ' ';
            spans.push_back({begin, end, true});
        }
    }
};

bool IsWhitespace(const char value)
{
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' || value == '\v';
}

bool IsIdentifierByte(const char value)
{
    const auto byte = static_cast<unsigned char>(value);
    return std::isalnum(byte) || value == '_' || byte >= 0x80;
}

std::size_t NewlineSize(const std::string_view source, const std::size_t index)
{
    if (index >= source.size())
        return 0;
    if (source[index] == '\n')
        return 1;
    if (source[index] == '\r')
        return index + 1 < source.size() && source[index + 1] == '\n' ? 2 : 1;
    return 0;
}

std::size_t EscapedNewlineSize(const std::string_view source, const std::size_t index)
{
    if (index >= source.size() || source[index] != '\\')
        return 0;
    auto newline = index + 1;
    while (newline < source.size() &&
           (source[newline] == ' ' || source[newline] == '\t' || source[newline] == '\v' || source[newline] == '\f'))
        ++newline;
    const auto size = NewlineSize(source, newline);
    return size > 0 ? newline - index + size : 0;
}

std::optional<std::size_t> RawQuoteAt(const std::string_view source, const std::size_t index)
{
    if (index > 0 && IsIdentifierByte(source[index - 1]))
        return std::nullopt;
    for (const std::string_view prefix : {"u8R\"", "uR\"", "UR\"", "LR\"", "R\""})
        if (source.substr(index, prefix.size()) == prefix)
            return index + prefix.size() - 1;
    return std::nullopt;
}

std::optional<std::size_t> RawStringEnd(const std::string_view source, const std::size_t quote)
{
    const auto delimiterBegin = quote + 1;
    auto opening = delimiterBegin;
    while (opening < source.size() && source[opening] != '(')
    {
        const auto value = static_cast<unsigned char>(source[opening]);
        if (opening - delimiterBegin >= 16 || value <= ' ' || value > '~' || value == ')' || value == '\\')
            return std::nullopt;
        ++opening;
    }
    if (opening == source.size())
        return std::nullopt;
    const auto terminator = ')' + std::string(source.substr(delimiterBegin, opening - delimiterBegin)) + '"';
    const auto closing = source.find(terminator, opening + 1);
    return closing == std::string_view::npos ? std::nullopt : std::optional{closing + terminator.size()};
}

std::optional<std::size_t> QuotedEnd(const std::string_view source, const std::size_t begin)
{
    for (auto index = begin + 1; index < source.size(); ++index)
    {
        if (source[index] == source[begin])
            return index + 1;
        if (NewlineSize(source, index) > 0)
            return std::nullopt;
        if (source[index] == '\\')
        {
            const auto continuation = EscapedNewlineSize(source, index);
            if (continuation > 0)
                index += continuation - 1;
            else if (++index == source.size())
                return std::nullopt;
        }
    }
    return std::nullopt;
}

std::size_t LineCommentEnd(const std::string_view source, std::size_t index)
{
    while (index < source.size())
    {
        if (const auto continuation = EscapedNewlineSize(source, index); continuation > 0)
            index += continuation;
        else if (const auto newline = NewlineSize(source, index); newline > 0)
            return index + newline;
        else
            ++index;
    }
    return index;
}

std::optional<std::size_t> BlockCommentEnd(const std::string_view source, const std::size_t index)
{
    const auto end = source.find("*/", index + 2);
    return end == std::string_view::npos ? std::nullopt : std::optional{end + 2};
}

std::size_t NumberEnd(const std::string_view source, std::size_t index)
{
    while (index + 1 < source.size())
    {
        const char next = source[index + 1];
        const bool separator = next == '\'' && index + 2 < source.size() && IsIdentifierByte(source[index + 2]);
        const bool exponentSign =
            (next == '+' || next == '-') && (source[index] == 'e' || source[index] == 'E' || source[index] == 'p' || source[index] == 'P');
        if (!IsIdentifierByte(next) && next != '.' && !separator && !exponentSign)
            break;
        ++index;
    }
    return index + 1;
}

// An unchanged index means ordinary code; absence means an unfinished opaque span.
std::optional<std::size_t> OpaqueEnd(const std::string_view source, const std::size_t index)
{
    if (source.substr(index, 2) == "//")
        return LineCommentEnd(source, index + 2);
    if (source.substr(index, 2) == "/*")
        return BlockCommentEnd(source, index);
    if (const auto quote = RawQuoteAt(source, index))
        return RawStringEnd(source, *quote);
    if (source[index] == '"' || source[index] == '\'')
        return QuotedEnd(source, index);
    if ((std::isdigit(static_cast<unsigned char>(source[index])) ||
            (source[index] == '.' && index + 1 < source.size() && std::isdigit(static_cast<unsigned char>(source[index + 1])))) &&
        (index == 0 || !IsIdentifierByte(source[index - 1])))
        return NumberEnd(source, index);
    const auto continuation = EscapedNewlineSize(source, index);
    if (continuation > 0)
    {
        const auto after = index + continuation;
        // Splicing adjacent tokens can create comment or raw-string delimiters.
        // Exact quotations still work; layout recovery does not reinterpret those joins.
        if (index > 0 && after < source.size() && !IsWhitespace(source[index - 1]) && !IsWhitespace(source[after]))
            return std::nullopt;
        return after;
    }
    return index;
}

std::optional<std::size_t> DirectiveStart(const std::string_view source, std::size_t index)
{
    while (index < source.size())
    {
        if (IsWhitespace(source[index]) && NewlineSize(source, index) == 0)
            ++index;
        else if (const auto continuation = EscapedNewlineSize(source, index); continuation > 0)
            index += continuation;
        else if (source.substr(index, 2) == "/*")
        {
            const auto end = BlockCommentEnd(source, index);
            if (!end)
                return std::nullopt;
            index = *end;
        }
        else
            return source[index] == '#' || source.substr(index, 2) == "%:" ? std::optional{index} : std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::size_t> DirectiveEnd(const std::string_view source, std::size_t index)
{
    while (index < source.size())
    {
        if (const auto newline = NewlineSize(source, index); newline > 0)
            return index + newline;
        const auto end = OpaqueEnd(source, index);
        if (!end)
            return std::nullopt;
        if (source.substr(index, 2) == "//")
            return end;
        index = *end > index ? *end : index + 1;
    }
    return index;
}

std::optional<NormalizedSource> Normalize(const std::string_view source)
{
    NormalizedSource normalized;
    bool linePrefix = true;
    for (std::size_t index = 0; index < source.size();)
    {
        if (linePrefix)
        {
            if (const auto directive = DirectiveStart(source, index))
            {
                const auto end = DirectiveEnd(source, *directive);
                if (!end)
                    return std::nullopt;
                normalized.AppendExact(source, index, *end);
                index = *end;
                continue;
            }
        }
        const auto end = OpaqueEnd(source, index);
        if (!end)
            return std::nullopt;
        if (*end > index)
        {
            const bool comment = source.substr(index, 2) == "//" || source.substr(index, 2) == "/*";
            if (comment)
                linePrefix = linePrefix || source.substr(index, *end - index).find_first_of("\r\n") != std::string_view::npos;
            else if (EscapedNewlineSize(source, index) == 0)
                linePrefix = false;
            normalized.AppendExact(source, index, *end);
            index = *end;
        }
        else if (IsWhitespace(source[index]))
        {
            const auto begin = index;
            while (index < source.size() && IsWhitespace(source[index]))
            {
                const auto newline = NewlineSize(source, index);
                index += newline > 0 ? newline : 1;
                if (newline > 0)
                {
                    linePrefix = true;
                    break;
                }
            }
            normalized.AppendWhitespace(begin, index);
        }
        else
        {
            normalized.AppendExact(source, index, index + 1);
            ++index;
            linePrefix = false;
        }
    }
    return normalized;
}

bool OpaqueBytesMatch(const std::string_view source, const NormalizedSource& normalizedSource, const std::string_view quotation,
    const NormalizedSource& normalizedQuotation, const std::size_t begin)
{
    for (std::size_t offset = 0; offset < normalizedQuotation.text.size(); ++offset)
    {
        const auto& actual = normalizedSource.spans[begin + offset];
        const auto& quoted = normalizedQuotation.spans[offset];
        if ((!actual.foldableWhitespace || !quoted.foldableWhitespace) &&
            source.substr(actual.begin, actual.end - actual.begin) != quotation.substr(quoted.begin, quoted.end - quoted.begin))
            return false;
    }
    return true;
}
}

std::optional<std::string> ResolveSourceQuotation(const std::string_view source, const std::string_view quotation)
{
    constexpr std::size_t MaximumRestoredBytes = 800;
    if (quotation.empty())
        return std::nullopt;
    if (quotation.size() <= MaximumRestoredBytes)
        if (const auto exact = source.find(quotation); exact != std::string_view::npos)
            return std::string(source.substr(exact, quotation.size()));
    const auto actual = Normalize(source);
    const auto quoted = Normalize(quotation);
    if (!actual || !quoted || quoted->text.empty())
        return std::nullopt;
    for (auto begin = actual->text.find(quoted->text); begin != std::string::npos; begin = actual->text.find(quoted->text, begin + 1))
    {
        const auto first = actual->spans[begin].begin;
        const auto last = actual->spans[begin + quoted->text.size() - 1].end;
        if (last - first <= MaximumRestoredBytes && OpaqueBytesMatch(source, *actual, quotation, *quoted, begin))
            return std::string(source.substr(first, last - first));
    }
    return std::nullopt;
}
}
