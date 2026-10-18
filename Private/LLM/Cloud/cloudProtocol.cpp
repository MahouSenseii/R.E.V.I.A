#include "LLM/Cloud/cloudProtocol.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace revia::llm::cloud
{

namespace
{
using json = nlohmann::json;

// Conversation text can hold malformed UTF-8 (a pasted binary, a truncated capture).
// nlohmann throws on it by default; replacing it keeps the request well-formed.
std::string Serialize(const json& value)
{
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::optional<std::uint32_t> TokenCount(const json& object, const char* key)
{
    if (!object.is_object() || !object.contains(key) || !object[key].is_number_unsigned())
        return std::nullopt;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(object[key].get<std::uint64_t>(), UINT32_MAX));
}

std::string StringField(const json& object, const char* key)
{
    return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : std::string{};
}

std::string ErrorMessage(const json& error)
{
    if (error.is_string())
        return error.get<std::string>();
    const std::string message = StringField(error, "message");
    return message.empty() ? std::string("The cloud model reported an error.") : message;
}

// Anthropic expects the conversation to open with the user and to alternate. Revia's
// history can hold two user turns in a row (a reply that failed) or start with her own
// line (a proactive opening), so adjacent turns are joined and a leading reply dropped.
std::vector<CloudMessage> Alternating(const std::vector<CloudMessage>& messages)
{
    std::vector<CloudMessage> result;
    for (const CloudMessage& message : messages)
    {
        if (message.content.empty() || (message.role != "user" && message.role != "assistant"))
            continue;
        if (result.empty() && message.role != "user")
            continue;
        if (!result.empty() && result.back().role == message.role)
            result.back().content += "\n\n" + message.content;
        else
            result.push_back(message);
    }
    return result;
}
} // namespace

std::optional<CloudProvider> ParseCloudProvider(const std::string& name)
{
    if (name == "openai-compatible")
        return CloudProvider::OpenAiCompatible;
    if (name == "anthropic")
        return CloudProvider::Anthropic;
    return std::nullopt;
}

const char* ToString(const CloudProvider provider)
{
    return provider == CloudProvider::Anthropic ? "anthropic" : "openai-compatible";
}

CloudWireRequest BuildCloudRequest(const CloudProvider provider, const CloudEndpoint& endpoint, const std::string& model,
    const int maxTokens, const CloudPayload& payload, const std::string& apiKey)
{
    CloudWireRequest request;
    request.headers.emplace_back("Content-Type", "application/json");
    request.headers.emplace_back("Accept", "text/event-stream");
    if (provider == CloudProvider::Anthropic)
    {
        request.path = endpoint.basePath + "/v1/messages";
        request.headers.emplace_back("x-api-key", apiKey);
        request.headers.emplace_back("anthropic-version", "2023-06-01");
        json messages = json::array();
        for (const CloudMessage& message : Alternating(payload.messages))
            messages.push_back({{"role", message.role}, {"content", message.content}});
        json body = {{"model", model}, {"max_tokens", maxTokens}, {"messages", std::move(messages)}, {"stream", true}};
        if (!payload.system.empty())
            body["system"] = payload.system;
        request.body = Serialize(body);
        return request;
    }

    request.path = endpoint.basePath + "/chat/completions";
    request.headers.emplace_back("Authorization", "Bearer " + apiKey);
    json messages = json::array();
    if (!payload.system.empty())
        messages.push_back({{"role", "system"}, {"content", payload.system}});
    for (const CloudMessage& message : payload.messages)
    {
        if (!message.content.empty() && (message.role == "user" || message.role == "assistant"))
            messages.push_back({{"role", message.role}, {"content", message.content}});
    }
    // include_usage asks for one last event with the token counts, so a cloud reply
    // reports its cost the same way a local one does.
    const json body = {{"model", model}, {"max_tokens", maxTokens}, {"messages", std::move(messages)}, {"stream", true},
        {"stream_options", {{"include_usage", true}}}};
    request.body = Serialize(body);
    return request;
}

std::vector<ServerSentEventParser::Event> ServerSentEventParser::Feed(const std::string_view chunk)
{
    std::vector<Event> completed;
    pending.append(chunk.data(), chunk.size());
    std::size_t start = 0;
    for (std::size_t newline = pending.find('\n'); newline != std::string::npos; newline = pending.find('\n', start))
    {
        std::string_view line(pending.data() + start, newline - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        TakeLine(line, completed);
        start = newline + 1;
    }
    pending.erase(0, start);
    return completed;
}

std::optional<ServerSentEventParser::Event> ServerSentEventParser::Finish()
{
    std::vector<Event> completed;
    if (!pending.empty())
    {
        std::string_view line(pending);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        TakeLine(line, completed);
        pending.clear();
    }
    TakeLine({}, completed);
    if (completed.empty())
        return std::nullopt;
    return completed.back();
}

void ServerSentEventParser::TakeLine(const std::string_view line, std::vector<Event>& completed)
{
    if (line.empty())
    {
        if (hasData)
            completed.push_back(std::move(current));
        current = {};
        hasData = false;
        return;
    }
    if (line.front() == ':')
        return;
    const std::size_t colon = line.find(':');
    const std::string_view field = line.substr(0, colon);
    std::string_view value = colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ')
        value.remove_prefix(1);
    if (field == "event")
    {
        current.name.assign(value.data(), value.size());
    }
    else if (field == "data")
    {
        if (hasData)
            current.data += '\n';
        current.data.append(value.data(), value.size());
        hasData = true;
    }
}

CloudStreamDelta DecodeCloudEvent(const CloudProvider provider, const ServerSentEventParser::Event& event)
{
    CloudStreamDelta delta;
    if (provider == CloudProvider::OpenAiCompatible && event.data == "[DONE]")
    {
        delta.done = true;
        return delta;
    }
    const json data = json::parse(event.data, nullptr, false);
    if (data.is_discarded() || !data.is_object())
        return delta;

    if (provider == CloudProvider::Anthropic)
    {
        const std::string type = StringField(data, "type");
        if (type == "message_start" && data.contains("message") && data["message"].contains("usage"))
        {
            delta.promptTokens = TokenCount(data["message"]["usage"], "input_tokens");
        }
        else if (type == "content_block_delta" && data.contains("delta"))
        {
            const json& block = data["delta"];
            const std::string kind = StringField(block, "type");
            if (kind == "text_delta")
                delta.text = StringField(block, "text");
            else if (kind == "thinking_delta")
                delta.reasoning = StringField(block, "thinking");
        }
        else if (type == "message_delta")
        {
            if (data.contains("delta"))
                delta.finishReason = StringField(data["delta"], "stop_reason");
            if (data.contains("usage"))
                delta.completionTokens = TokenCount(data["usage"], "output_tokens");
        }
        else if (type == "message_stop")
        {
            delta.done = true;
        }
        else if (type == "error")
        {
            delta.error = data.contains("error") ? ErrorMessage(data["error"]) : "The cloud model reported an error.";
        }
        return delta;
    }

    if (data.contains("error"))
    {
        delta.error = ErrorMessage(data["error"]);
        return delta;
    }
    if (data.contains("usage") && data["usage"].is_object())
    {
        delta.promptTokens = TokenCount(data["usage"], "prompt_tokens");
        delta.completionTokens = TokenCount(data["usage"], "completion_tokens");
    }
    if (data.contains("choices") && data["choices"].is_array() && !data["choices"].empty())
    {
        const json& choice = data["choices"][0];
        if (choice.contains("delta") && choice["delta"].is_object())
        {
            const json& piece = choice["delta"];
            delta.text = StringField(piece, "content");
            // Servers that expose thinking name it either way.
            delta.reasoning = StringField(piece, "reasoning_content");
            if (delta.reasoning.empty())
                delta.reasoning = StringField(piece, "reasoning");
        }
        delta.finishReason = StringField(choice, "finish_reason");
    }
    return delta;
}

} // namespace revia::llm::cloud
