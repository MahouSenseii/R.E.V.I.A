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

    nlohmann::json BuildMessages(
        const aiProfile& profile,
        const std::vector<conversationMessage>& context,
        const std::vector<float>& queryEmbedding = {},
        const std::string& embeddingModel = "",
        std::vector<latencySample>* timings = nullptr
    ) const;
    std::string BuildMemoryBlock(const std::string& query = "") const;

private:

    longTermMemory memory;
};
