#pragma once

#include "Identity/promptMarkers.h"

#include <nlohmann/json.hpp>

#include <string>

namespace revia::tests
{

// The runtime block at the start of a user message, without its markers, or empty.
inline std::string RuntimeBlockOf(const std::string& content)
{
    namespace markers = revia::identity::markers;
    const std::string open = std::string(markers::RuntimeTurnContext) + "\n";
    const std::string close = "\n" + std::string(markers::RuntimeTurnContextEnd);
    if (content.rfind(open, 0) != 0) return {};
    const std::size_t end = content.find(close, open.size());
    return end == std::string::npos ? std::string() : content.substr(open.size(), end - open.size());
}

// A user message as the user wrote it, with any leading runtime block removed.
inline std::string WithoutRuntimeBlock(const std::string& content)
{
    namespace markers = revia::identity::markers;
    if (content.rfind(std::string(markers::RuntimeTurnContext), 0) != 0) return content;
    const std::string close = std::string(markers::RuntimeTurnContextEnd) + "\n\n";
    const std::size_t end = content.find(close);
    return end == std::string::npos ? content : content.substr(end + close.size());
}

// Everything the runtime wrote into a chat request: the system messages plus the runtime
// block at the start of the newest user message.
inline std::string RuntimeAuthoredText(const nlohmann::json& request)
{
    std::string text;
    const auto& messages = request.at("messages");
    for (const auto& message : messages)
    {
        if (message.value("role", "") == "system") text += message.at("content").get<std::string>();
    }
    if (!messages.empty() && messages.back().value("role", "") == "user")
    {
        text += "\n\n" + RuntimeBlockOf(messages.back().value("content", ""));
    }
    return text;
}

} // namespace revia::tests
