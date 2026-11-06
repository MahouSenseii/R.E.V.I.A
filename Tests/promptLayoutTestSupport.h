#pragma once

#include "Identity/promptMarkers.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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

// The person's words exactly as sent: the runtime block removed, and the host notes the
// runtime places after the words (where the model weighs them most) cut off. A change to the
// words themselves still fails any comparison made on this.
inline std::string UserWordsOf(const std::string& content)
{
    std::string words = WithoutRuntimeBlock(content);
    std::size_t cut = std::string::npos;
    for (const char* note : {"\n\nWhat was said or done before is only what this chat shows",
             "\n\nYour last request to use your agents did not start a task", "\n\nEarlier in this chat you were talking with someone else",
             "\n\nThis person gave their name partway through this chat",
             "\n\nAgents unavailable this turn", "\nTools are offered with this request."})
        cut = std::min(cut, words.find(note));
    if (cut == std::string::npos)
        return words;
    // The prompt separates the words from the notes with blank lines; those are not the words.
    words.erase(cut);
    while (!words.empty() && words.back() == '\n')
        words.pop_back();
    return words;
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
