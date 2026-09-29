#pragma once

#include "Library/structLibrary.h"

#include <string>

namespace revia::llm
{

// Which kind of server answers a chat endpoint.
//
// They all speak OpenAI's chat completions, which is why one HTTP client serves every
// one of them. They differ in the extras around it -- a health endpoint, a tokenizer,
// llama.cpp's sampling and template knobs -- and a request that assumes the extras
// fails on a server without them. The capabilities below are what the client checks
// before it relies on any of them.
enum class ProviderKind
{
    LlamaCpp,
    Ollama,
    LMStudio,
    // Anything else that speaks the OpenAI chat API: vLLM, TabbyAPI, KoboldCpp, a
    // gateway, or a hosted service on a remote host.
    OpenAICompatible
};

struct ProviderCapabilities
{
    ProviderKind kind = ProviderKind::LlamaCpp;
    // What the status line and log call it.
    const char* displayName = "llama.cpp";
    // GET /health answers before a model is loaded.
    bool healthEndpoint = true;
    // GET /props reports context size, slots and modalities.
    bool propsEndpoint = true;
    // POST /tokenize counts with the model's own tokenizer.
    bool tokenizeEndpoint = true;
    // response_format json_schema is enforced by the server.
    bool jsonSchema = true;
    // dry_* sampling fields are understood rather than rejected.
    bool drySampling = true;
    // chat_template_kwargs, which is how thinking is switched on and off.
    bool chatTemplateKwargs = true;
    // cache_prompt, llama.cpp's prefix-cache switch.
    bool cachePrompt = true;
    // /v1/models lists exactly the loaded model, so a mismatch means the wrong server.
    bool strictModelList = true;
};

// The capabilities of the backend a settings file names: "LLamaCpp", "Ollama",
// "LMStudio", "OpenAI" or "CustomHttp". Unknown names get the plainest profile.
[[nodiscard]] ProviderCapabilities CapabilitiesFor(const std::string& backendName);
// Whether the name is one the chat client can serve at all.
[[nodiscard]] bool IsChatBackend(const std::string& backendName);
// Whether a host names this machine. What decides if a request stays local.
[[nodiscard]] bool IsLoopbackHost(const std::string& host);
// Whether a tier's requests leave this machine.
[[nodiscard]] bool IsCloudEndpoint(const llmSettings& settings);

} // namespace revia::llm
