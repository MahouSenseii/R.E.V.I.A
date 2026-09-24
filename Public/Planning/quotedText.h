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

// One left-to-right interpretation for content extraction and authority decisions.
// Supports straight/curly single and double quotes, mixed nesting, and escaped
// delimiters inside quotes. Apostrophes inside words are not opening quotes;
// trailing possessives/dangling closers are ambiguous and conservatively refused.
// Spans are outer quotations, in source order; nested quotations remain payload.
// An unmatched delimiter or excessive nesting returns complete=false with no spans
// or instruction. A partial/unprocessed parse must never become authority.
[[nodiscard]] ParsedQuotation ParseQuotation(const std::string& request);

// First complete outer span in source order. No span if the whole parse is invalid.
[[nodiscard]] QuotedSpan FindQuoted(const std::string& request);

// All payloads removed, outer delimiters retained. Returns empty for invalid input.
[[nodiscard]] std::string WithoutQuotedPayload(const std::string& request);

} // namespace revia::planning
