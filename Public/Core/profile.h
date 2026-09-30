#pragma once

#include "Agents/answerObligation.h"
#include <map>
#include <string>
#include <utility>
#include <vector>

struct aiProfile
{
    std::string id = "assistant";
    std::string displayName = "Assistant";
    // Shown in the profile picker so a profile can be recognised without reading the
    // whole system prompt. Optional: an empty description is a valid profile.
    std::string description;
    std::string systemPrompt = "You are a helpful local AI assistant.";

    // H3: when false, user input is not written to long-term memory.
    bool bMemoryEnabled = true;

    bool bHasTemperatureOverride = false;
    bool bHasMaxTokensOverride = false;

    float temperature = 0.7f;
    int maxTokens = 512;

    // Trait-name overrides retain built-in defaults for omitted names; Identity validates names.
    std::map<std::string, float> personalityBaseline;

    // Declared subject/strength baselines seed only absent opinions; earned preferences survive.
    std::vector<std::pair<std::string, float>> preferences;

    // Authored answer obligation, independent of earned state; older profiles load as Balanced.
    AnswerObligationMode answerObligation = AnswerObligationMode::Balanced;
};
