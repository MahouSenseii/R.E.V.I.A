#pragma once

#include "Core/conversationContext.h"
#include "Core/messageRouter.h"

#include <stop_token>
#include <string>

namespace revia::agents
{

struct HistoryCompactionResult
{
    bool succeeded = false;
    std::string summary;
    std::string reason;
    double elapsedMilliseconds = 0.0;
};

// Folds the oldest part of the conversation into one running summary.
//
// Runs on the Main model at background priority, after a reply has been delivered, so a
// person never waits on it: a message of theirs preempts it, and it is simply tried again
// after that turn. The summary replaces the turns it covers, so what matters is what later
// turns depend on -- names, facts, decisions, promises, open questions -- and that it
// invents none of them.
class HistoryCompactor
{
public:
    [[nodiscard]] HistoryCompactionResult Compact(
        const messageRouter& router,
        const conversationContext::CompactionJob& job,
        std::stop_token stopToken = {}) const;

    // The instructions, shared with the model call so the two cannot drift apart.
    [[nodiscard]] static const char* SystemPrompt();
    // A strict schema, so the summary is bounded by the grammar rather than by trust.
    [[nodiscard]] static const char* ResponseSchema();
    // The bounded material handed to the model, exposed so it can be tested without one.
    [[nodiscard]] static std::string BuildEnvelope(const conversationContext::CompactionJob& job);
    [[nodiscard]] static HistoryCompactionResult Parse(const std::string& raw);
};

} // namespace revia::agents
