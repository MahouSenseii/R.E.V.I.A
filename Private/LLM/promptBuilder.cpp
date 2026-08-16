#include "LLM/promptBuilder.h"

#include <chrono>

namespace
{
    double ElapsedMilliseconds(const std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }
}

promptBuilder::promptBuilder() = default;

promptBuilder::~promptBuilder() = default;

nlohmann::json promptBuilder::BuildMessages(const aiProfile& profile,
const std::vector<conversationMessage>& context,
const std::vector<float>& queryEmbedding,
const std::string& embeddingModel,
std::vector<latencySample>* timings) const
{
    nlohmann::json messages = nlohmann::json::array();

    std::string systemContent = profile.systemPrompt;

    std::string retrievalQuery;
    for (auto message = context.rbegin(); message != context.rend(); ++message)
    {
        if (message->role == "user" && !message->content.empty())
        {
            retrievalQuery = message->content;
            break;
        }
    }

    const auto retrievalStarted = std::chrono::steady_clock::now();
    const std::string memoryBlock = memory.BuildPromptBlock(
        retrievalQuery,
        6,
        queryEmbedding,
        embeddingModel);
    if (timings)
    {
        timings->push_back({"memory_retrieval", ElapsedMilliseconds(retrievalStarted)});
    }

    const auto assemblyStarted = std::chrono::steady_clock::now();

    if (!memoryBlock.empty())
    {
        if (!systemContent.empty())
        {
            systemContent += "\n\n";
        }

        systemContent += memoryBlock;
    }

    if (!systemContent.empty())
    {
        messages.push_back({
            {"role", "system"},
            {"content", systemContent}
        });
    }

    for (const conversationMessage& message : context)
    {
        if (message.content.empty())
        {
            continue;
        }

        messages.push_back({
            {"role", message.role},
            {"content", message.content}
        });
    }

    if (timings)
    {
        timings->push_back({"prompt_assembly", ElapsedMilliseconds(assemblyStarted)});
    }
    return messages;
}

std::string promptBuilder::BuildMemoryBlock(const std::string& query) const
{
    return memory.BuildPromptBlock(query);
}
