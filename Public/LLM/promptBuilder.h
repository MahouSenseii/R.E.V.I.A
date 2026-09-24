#pragma once

#include "Library/structLibrary.h"
#include "Memory/longTermMemory.h"
#include "LLM/privateMemoryAccess.h"

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
        std::vector<promptSection>* sections = nullptr,
        revia::llm::PrivateMemoryAccess memoryAccess = revia::llm::PrivateMemoryAccess::ProfileSetting,
        // Placed after the newest user message instead of in the system message, for the
        // few lines this turn's answer must follow. A small model weighs the end of the
        // prompt most; five thousand characters of posture earlier, her own conclusion
        // lost to the model's habit of saying it has no voice and cannot sing.
        const std::string& replyNote = "",
        // When the newest message is the user's, send posture and memories at its start
        // instead of in the system message, so the system message is identical between
        // turns and the backend can reuse its cached prompt.
        bool stablePrefix = false
    ) const;
    std::string BuildMemoryBlock(const std::string& query = "") const;
    // The saved memories closest to `query`, for a caller deciding whether something is
    // already known. Ranked by the embedding when one is given, by text otherwise.
    std::string BuildRelatedMemoryBlock(
        const std::string& query,
        const std::vector<float>& queryEmbedding,
        const std::string& embeddingModel,
        std::size_t maxEntries) const;

private:

    longTermMemory memory;
};
