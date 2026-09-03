#pragma once

#include "Library/structLibrary.h"
#include "Memory/longTermMemory.h"

#include <nlohmann/json.hpp>
#include <vector>

class promptBuilder
{
public:
    promptBuilder();
    ~promptBuilder();

    // posture is Revia's own current response posture, already formatted. It is her
    // state, not a claim about the user's: the affect controller describes how Revia is
    // approaching this turn, and passing it in is what makes that visible to the model
    // rather than only to the status chip and the speech rate.
    nlohmann::json BuildMessages(
        const aiProfile& profile,
        const std::vector<conversationMessage>& context,
        const std::vector<float>& queryEmbedding = {},
        const std::string& embeddingModel = "",
        std::vector<latencySample>* timings = nullptr,
        const std::string& posture = "",
        // Filled, in prompt order, with what the system message and history were made
        // of. Measured here because this is the only place that still sees the pieces
        // separately -- one line further on they are a single concatenated string, and
        // no consumer can tell which subsystem paid for which part of it.
        std::vector<promptSection>* sections = nullptr
    ) const;
    std::string BuildMemoryBlock(const std::string& query = "") const;

private:

    longTermMemory memory;
};
