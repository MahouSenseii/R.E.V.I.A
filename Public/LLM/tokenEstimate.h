#pragma once

#include <cstddef>
#include <string>

namespace revia::llm
{

// Conservative content allowance: one token per UTF-8 byte, including whitespace.
// This intentionally sacrifices history compared with a calibrated language heuristic.
// It bounds content for byte-fallback tokenizers; it is not an exact count of a model
// or its serialized chat template. The caller reserves framing/generation overhead
// and performs at most one smaller retry on an explicit backend context overflow.
[[nodiscard]] std::size_t EstimateTokens(const std::string& text);

// Conservative per-message framing allowance, in addition to a request-wide reserve.
// Templates differ, so an explicit server overflow still gets one bounded recovery.
inline constexpr std::size_t ChatTemplateTokensPerMessage = 32;

// Shortens `text` until its byte allowance fits `tokenBudget`, keeping the beginning
// and the end and putting `marker` between them.
//
// The marker is included in the allowance and cuts preserve valid UTF-8 boundaries.
// The function is pure and does not make a network call.
[[nodiscard]] std::string CompactToTokenBudget(
    const std::string& text,
    std::size_t tokenBudget,
    const std::string& marker);

} // namespace revia::llm
