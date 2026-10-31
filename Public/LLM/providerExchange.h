#pragma once

#include <string>

namespace revia::llm
{

// The exact exchange behind one reply from a local provider: the request body it was sent
// and the text it streamed back. Held in memory with the reply so an owner judgment can be
// bound to the request that produced the displayed answer. Nothing writes it to disk unless
// the owner asks to keep that judged reply.
struct ProviderExchange
{
    std::string requestBody;
    // The concatenated content deltas, before any visible-text cleanup or repair.
    std::string responseText;
    std::string finishReason;
    bool completeStream = false;
    // The stream asked for tool calls, even if this step could not run them.
    bool requestedTools = false;
    std::string model;
    // SHA-256 of the chat template the server reported at its last health check; empty when
    // it has not reported one.
    std::string templateSha256;
};

} // namespace revia::llm
