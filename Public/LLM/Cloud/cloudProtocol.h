#pragma once

#include "LLM/Cloud/cloudTransport.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revia::llm::cloud
{

enum class CloudProvider
{
    OpenAiCompatible,
    Anthropic
};

[[nodiscard]] std::optional<CloudProvider> ParseCloudProvider(const std::string& name);
[[nodiscard]] const char* ToString(CloudProvider provider);

struct CloudMessage
{
    // "user" or "assistant".
    std::string role;
    std::string content;
};

// Everything a cloud request carries besides the model name and the key. Built by the
// privacy layer; the protocol layer only formats it.
struct CloudPayload
{
    std::string system;
    std::vector<CloudMessage> messages;
};

struct CloudWireRequest
{
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

// The HTTP request for one streamed chat turn.
//   OpenAI-compatible: POST {base}/chat/completions, Authorization: Bearer <key>.
//   Anthropic:         POST {base}/v1/messages, x-api-key and anthropic-version 2023-06-01.
// The key goes into a header and nowhere else; the body never contains it.
[[nodiscard]] CloudWireRequest BuildCloudRequest(CloudProvider provider, const CloudEndpoint& endpoint, const std::string& model,
    int maxTokens, const CloudPayload& payload, const std::string& apiKey);

// Splits a text/event-stream body into events, however the bytes are chunked on the
// wire. Only `event:` and `data:` fields matter here; comments and ids are ignored.
class ServerSentEventParser
{
public:
    struct Event
    {
        std::string name;
        std::string data;
    };

    // Returns every event completed by this chunk.
    [[nodiscard]] std::vector<Event> Feed(std::string_view chunk);
    // A final event that was not followed by a blank line.
    [[nodiscard]] std::optional<Event> Finish();

private:
    void TakeLine(std::string_view line, std::vector<Event>& completed);

    std::string pending;
    Event current;
    bool hasData = false;
};

// What one decoded stream event added to the reply.
struct CloudStreamDelta
{
    std::string text;
    std::string reasoning;
    std::string finishReason;
    std::optional<std::uint32_t> promptTokens;
    std::optional<std::uint32_t> completionTokens;
    // An error the server reported inside the stream (Anthropic `error` events, or an
    // OpenAI-compatible server that sends {"error": ...} as data).
    std::string error;
    bool done = false;
};

// Decodes one event for the provider. Unknown event types decode to an empty delta, so a
// server that adds new ones does not break the reply.
[[nodiscard]] CloudStreamDelta DecodeCloudEvent(CloudProvider provider, const ServerSentEventParser::Event& event);

} // namespace revia::llm::cloud
