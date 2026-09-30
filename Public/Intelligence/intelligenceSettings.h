#pragma once

#include <string>

// Additional model endpoints share the profile and conversation state.
// Hardware placement is resolved at runtime.
struct modelTierSettings
{
    bool bEnabled = false;
    std::string host = "127.0.0.1";
    int port = 0;
    std::string modelName;
    std::string modelPath;
    bool bVisionEnabled = false;
    std::string multimodalProjectorPath;
    int contextSize = 8192;
    int maxTokens = 384;
    float temperature = 0.75F;
    int startupTimeoutSeconds = 120;
    bool bWarmAtStartup = true;
    // Allow idle eviction; false keeps the tier resident for the session.
    bool bOnDemand = false;
    // Idle time before eviction and minimum residency after loading, to avoid reload churn.
    int idleGraceSeconds = 300;
    int minimumResidencySeconds = 60;
};

struct intelligenceSettings
{
    bool bEnabled = true;
    modelTierSettings fast = {
        true,
        "127.0.0.1",
        8082,
        "Qwen3.5-0.8B-Q4_K_M.gguf",
        "Models/Qwen3.5-0.8B-Q4_K_M.gguf",
        false,
        "",
        8192,
        256,
        0.78F,
        90,
        true};
    modelTierSettings expert = {
        true,
        "127.0.0.1",
        8083,
        "Qwen3-VL-8B-Instruct-Unredacted-MAX.Q4_K_M.gguf",
        "Models/Qwen3-VL-8B-Instruct-Unredacted-MAX.Q4_K_M.gguf",
        true,
        "Models/Qwen3-VL-8B-Instruct-Unredacted-MAX.mmproj-q8_0.gguf",
        8192,
        1024,
        0.72F,
        180,
        true};
};
