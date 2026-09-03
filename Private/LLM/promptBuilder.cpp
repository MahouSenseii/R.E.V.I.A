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
std::vector<latencySample>* timings,
const std::string& posture,
std::vector<promptSection>* sections) const
{
    nlohmann::json messages = nlohmann::json::array();

    const auto record = [sections](const char* name, const std::string& text,
        const bool stable)
    {
        if (sections && !text.empty())
        {
            sections->push_back({name, text.size(), stable});
        }
    };

    std::string systemContent = profile.systemPrompt;
    // The only part of the system message that is byte-identical between turns, and so
    // the only part a prefix cache can ever reuse. Everything recorded after this is
    // recorded in the order it is concatenated, because that order decides how much of
    // the cache survives.
    record("system_prompt", profile.systemPrompt, true);
    if (!posture.empty())
    {
        if (!systemContent.empty())
        {
            systemContent += "\n\n";
        }
        systemContent += posture;
    }
    // Not stable: the state packet opens with an emotion percentage that moves every
    // turn, and the screen observation carries an age in seconds. Both sit at the front
    // of this block, so the whole of it -- and everything after it -- is re-evaluated.
    record("posture", posture, false);

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
    const std::string memoryBlock = profile.bMemoryEnabled
        ? memory.BuildPromptBlock(
            retrievalQuery,
            6,
            queryEmbedding,
            embeddingModel)
        : std::string{};
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
    record("memory_block", memoryBlock, false);

    if (!systemContent.empty())
    {
        messages.push_back({
            {"role", "system"},
            {"content", systemContent}
        });
    }

    std::size_t historyCharacters = 0;
    for (const conversationMessage& message : context)
    {
        if (message.content.empty())
        {
            continue;
        }

        historyCharacters += message.content.size();
        messages.push_back({
            {"role", message.role},
            {"content", message.content}
        });
    }
    if (sections && historyCharacters > 0)
    {
        // One entry rather than one per message: the question this answers is how much
        // of the prompt is history, not which turn contributed what. It is listed as
        // unstable because it grows by two messages every turn, which is also why the
        // prompt drifts upward across a session.
        sections->push_back({"history", historyCharacters, false});
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
