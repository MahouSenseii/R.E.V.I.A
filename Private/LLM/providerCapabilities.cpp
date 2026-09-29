#include "LLM/providerCapabilities.h"

#include <algorithm>
#include <cctype>

namespace revia::llm
{

namespace
{
std::string Lowered(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}
}

ProviderCapabilities CapabilitiesFor(const std::string& backendName)
{
    const std::string name = Lowered(backendName);
    ProviderCapabilities capabilities;
    if (name == "llamacpp" || name == "llama.cpp" || name == "llama-cpp")
    {
        return capabilities;
    }
    // Everything below the line is the OpenAI chat API and nothing more, until a
    // server proves otherwise. Ollama and LM Studio both enforce a JSON schema; both
    // list only the models they have, so a mismatch still means "not pulled".
    capabilities.kind = ProviderKind::OpenAICompatible;
    capabilities.displayName = "OpenAI-compatible server";
    capabilities.healthEndpoint = false;
    capabilities.propsEndpoint = false;
    capabilities.tokenizeEndpoint = false;
    capabilities.drySampling = false;
    capabilities.chatTemplateKwargs = false;
    capabilities.cachePrompt = false;
    if (name == "ollama")
    {
        capabilities.kind = ProviderKind::Ollama;
        capabilities.displayName = "Ollama";
    }
    else if (name == "lmstudio" || name == "lm studio" || name == "lm-studio")
    {
        capabilities.kind = ProviderKind::LMStudio;
        capabilities.displayName = "LM Studio";
    }
    else
    {
        // A gateway or hosted service lists every model it can route to, and some
        // list none. The name in settings is what the request carries either way.
        capabilities.strictModelList = false;
    }
    return capabilities;
}

bool IsChatBackend(const std::string& backendName)
{
    const std::string name = Lowered(backendName);
    return name == "llamacpp" || name == "llama.cpp" || name == "llama-cpp" ||
        name == "ollama" || name == "lmstudio" || name == "lm studio" ||
        name == "lm-studio" || name == "openai" || name == "openaicompatible" ||
        name == "openai-compatible" || name == "customhttp" || name == "custom";
}

bool IsLoopbackHost(const std::string& host)
{
    const std::string lowered = Lowered(host);
    return lowered == "127.0.0.1" || lowered == "localhost" || lowered == "::1" ||
        lowered == "[::1]" || lowered.rfind("127.", 0) == 0;
}

bool IsCloudEndpoint(const llmSettings& settings)
{
    return settings.bTreatAsRemote || !IsLoopbackHost(settings.host);
}

} // namespace revia::llm
