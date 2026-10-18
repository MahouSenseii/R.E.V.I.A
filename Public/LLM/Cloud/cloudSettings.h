#pragma once

#include <string>

namespace revia::llm::cloud
{

// settings.json llm.cloud. Off unless the owner turns it on and the named environment
// variable holds a key. The key itself is never a setting: it is read from the
// environment at request time, so it cannot end up in a settings file, a backup of
// one, or a log line that prints settings.
struct CloudModelSettings
{
    bool bEnabled = false;
    // "openai-compatible" (POST {baseUrl}/chat/completions) or "anthropic"
    // (POST {baseUrl}/v1/messages).
    std::string provider = "openai-compatible";
    std::string baseUrl;
    std::string model;
    std::string apiKeyEnvironmentVariable;
    // "never", "ask" (only when the user explicitly asks the cloud) or "hardTurns" (also
    // the turns the router would have sent to Expert).
    std::string useFor = "never";
    int maxTokens = 1024;
    int timeoutSeconds = 60;
};

} // namespace revia::llm::cloud
