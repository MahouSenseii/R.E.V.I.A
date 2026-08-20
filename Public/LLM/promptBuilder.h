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
        const std::string& posture = ""
    ) const;
    std::string BuildMemoryBlock(const std::string& query = "") const;

private:

    longTermMemory memory;
};
