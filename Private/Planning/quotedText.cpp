#include "Planning/quotedText.h"

#include <array>
#include <cctype>
#include <string_view>

namespace revia::planning
{
namespace
{
struct Delimiter
{
    std::string_view open;
    std::string_view close;
    bool single;
};
constexpr std::array<Delimiter, 4> Delimiters{{
    {"\"", "\"", false}, {"'", "'", true},
    {"\xE2\x80\x9C", "\xE2\x80\x9D", false},
    {"\xE2\x80\x98", "\xE2\x80\x99", true}
}};

bool WordByte(const char value)
{
    const auto byte = static_cast<unsigned char>(value);
    return byte >= 128 || std::isalnum(byte) != 0 || byte == '_';
}

bool Matches(const std::string& text, const std::size_t at, const std::string_view delimiter)
{
    return text.compare(at, delimiter.size(), delimiter) == 0;
}
} // namespace

ParsedQuotation ParseQuotation(const std::string& request)
{
    ParsedQuotation parsed;
    parsed.instruction.reserve(request.size());
    // Complexity is linear in input length, with a fixed upper nesting depth. Reaching
    // the depth limit refuses the entire parse; there is no unprocessed suffix.
    std::array<std::size_t, 32> stack{};
    std::size_t depth = 0;
    QuotedSpan outer;
    const auto invalid = [] { return ParsedQuotation{false, {}, {}}; };
    for (std::size_t at = 0; at < request.size();)
    {
        if (depth != 0 && request[at] == '\\')
        {
            // Consume the next byte (or whole UTF-8 quote) as data. Paired
            // backslashes consume each other, so escaping has the right parity.
            ++at;
            if (at < request.size())
            {
                std::size_t length = 1;
                for (const auto& delimiter : Delimiters)
                {
                    if (Matches(request, at, delimiter.open)) length = delimiter.open.size();
                    if (Matches(request, at, delimiter.close)) length = delimiter.close.size();
                }
                at += length;
            }
            continue;
        }
        if (depth != 0)
        {
            const auto& active = Delimiters[stack[depth - 1]];
            const std::size_t after = at + active.close.size();
            const bool apostrophe = active.single && at > 0 && WordByte(request[at - 1]) &&
                after < request.size() && WordByte(request[after]);
            if (Matches(request, at, active.close) && !apostrophe)
            {
                --depth;
                if (depth == 0)
                {
                    outer.end = at;
                    outer.after = after;
                    parsed.spans.push_back(outer);
                    parsed.instruction.append(active.close);
                }
                at = after;
                continue;
            }
        }
        bool opened = false;
        for (std::size_t kind = 0; kind < Delimiters.size(); ++kind)
        {
            const auto& delimiter = Delimiters[kind];
            if (!Matches(request, at, delimiter.open)) continue;
            if (delimiter.single && at > 0 && WordByte(request[at - 1]))
            {
                const auto after = at + delimiter.open.size();
                if (depth == 0 && (after == request.size() || !WordByte(request[after])))
                    return invalid();
                continue;
            }
            if (depth == stack.size()) return invalid();
            if (depth == 0)
            {
                outer = {true, at + delimiter.open.size(), 0, at, 0};
                parsed.instruction.append(delimiter.open);
            }
            stack[depth++] = kind;
            at += delimiter.open.size();
            opened = true;
            break;
        }
        if (opened) continue;
        // A dangling closer is ambiguous with a trailing possessive. Only internal
        // apostrophes are unambiguous outside quotes; refuse the other spelling.
        if (Matches(request, at, Delimiters[2].close) ||
            (Matches(request, at, Delimiters[3].close) &&
                (at == 0 || !WordByte(request[at - 1]) ||
                    (depth == 0 && (at + 3 >= request.size() || !WordByte(request[at + 3]))))))
            return invalid();
        if (depth == 0) parsed.instruction.push_back(request[at]);
        ++at;
    }
    if (depth != 0) return invalid();
    return parsed;
}

QuotedSpan FindQuoted(const std::string& request)
{
    const auto parsed = ParseQuotation(request);
    return parsed.spans.empty() ? QuotedSpan{} : parsed.spans.front();
}

std::string WithoutQuotedPayload(const std::string& request)
{
    return ParseQuotation(request).instruction;
}

} // namespace revia::planning
