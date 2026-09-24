#include "LLM/tokenEstimate.h"


namespace revia::llm
{

namespace
{

// Cutting text by byte offset is how a budget in tokens becomes a string that is not
// text at all.
//
// A three-byte CJK character or a four-byte emoji sliced down the middle leaves a
// dangling continuation byte, and the result is no longer valid UTF-8. nlohmann::json
// throws on that when the request is serialized, which happens on the conversation
// worker -- so a long enough Chinese message could take the desktop app down rather
// than produce a short reply. The budget arithmetic below is in bytes because the
// conservative allowance is; these two move the chosen offset to the nearest character
// boundary before anything is copied.
//
// A continuation byte is 10xxxxxx. Anything else starts a character.
bool IsContinuation(const char byte)
{
    return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
}

// The boundary at or before `index`, for the end of a prefix.
std::size_t BoundaryAtOrBefore(const std::string& text, std::size_t index)
{
    if (index >= text.size()) return text.size();
    while (index > 0 && IsContinuation(text[index])) --index;
    return index;
}

// The boundary at or after `index`, for the start of a suffix.
std::size_t BoundaryAtOrAfter(const std::string& text, std::size_t index)
{
    while (index < text.size() && IsContinuation(text[index])) ++index;
    return index;
}

} // namespace

std::size_t EstimateTokens(const std::string& text)
{
    // Byte-fallback tokenizers cannot need more than one content token per byte.
    // Whitespace, rare strings and multilingual text all consume the same bound.
    // Chat-template tokens are reserved separately by the request fitter.
    return text.size();
}

std::string CompactToTokenBudget(
    const std::string& text,
    const std::size_t tokenBudget,
    const std::string& marker)
{
    if (text.size() <= tokenBudget) return text;
    if (tokenBudget == 0) return {};
    // The byte bound is independent of language and includes the marker. Tiny
    // budgets retain the end rather than spending their whole allowance on a label.
    if (marker.size() >= tokenBudget || tokenBudget - marker.size() <= 4)
        return text.substr(BoundaryAtOrAfter(text, text.size() - tokenBudget));
    const std::size_t available = tokenBudget - marker.size();
    const std::size_t prefixAllowance = available * 2 / 3;
    const std::size_t prefix = BoundaryAtOrBefore(text, prefixAllowance);
    const std::size_t suffixStart =
        BoundaryAtOrAfter(text, text.size() - (available - prefixAllowance));
    return text.substr(0, prefix) + marker + text.substr(suffixStart);
}

} // namespace revia::llm