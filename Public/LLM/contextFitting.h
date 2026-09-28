#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace revia::llm
{

// What a piece of text costs in the model's own tokens.
//
// Empty means "no tokenizer available": the fitter then spends EstimateTokens, which
// never under-counts but charges ordinary English about five times what it really
// costs. That price was paid in history. An 8K context held about 6.8 KB of text, her
// system prompt alone is close to 5 KB, and on most turns the conversation before the
// current message was dropped whole -- she forgot what had been said a minute earlier.
using TokenCounter = std::function<std::size_t(const std::string&)>;

inline constexpr std::size_t MaximumPromptBytes = 256 * 1024;
// Generation prompt and chat-template framing beyond the per-message allowance.
inline constexpr int ContextReserveTokens = 384;

// Fits a text-only chat request into `contextTokens`, keeping the system message and
// the newest turn and as much of the conversation before it as fits.
//
// `count` is the model's tokenizer when there is one. It is never consulted when the
// byte bound already fits, so a short request costs no tokenizer calls at all. Returns
// an empty array when nothing can fit.
[[nodiscard]] nlohmann::json BoundMessagesForContext(
    const nlohmann::json& messages,
    int contextTokens,
    int responseTokens,
    std::size_t maximumPromptTokens = MaximumPromptBytes,
    const TokenCounter& count = {});

} // namespace revia::llm
