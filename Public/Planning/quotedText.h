#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace revia::planning
{

// Byte positions in the original request. [begin, end) is the exact payload;
// [opening, after) additionally includes its quotation delimiters.
struct QuotedSpan
{
    bool found = false;
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t opening = 0;
    std::size_t after = 0;
};

struct ParsedQuotation
{
    bool complete = true;
    std::vector<QuotedSpan> spans;
    std::string instruction;
};

// Parses straight/curly single and double quotes, mixed nesting, and escaped delimiters;
// Outer spans preserve source order; nested quotes remain payload and word apostrophes are not openings.
// Ambiguous possessives, unmatched delimiters, or excessive nesting return incomplete with no spans/instruction.
// Partial parses never supply authority.
[[nodiscard]] ParsedQuotation ParseQuotation(const std::string& request);

// First complete outer span in source order. No span if the whole parse is invalid.
[[nodiscard]] QuotedSpan FindQuoted(const std::string& request);

// All payloads removed, outer delimiters retained. Returns empty for invalid input.
[[nodiscard]] std::string WithoutQuotedPayload(const std::string& request);

} // namespace revia::planning
