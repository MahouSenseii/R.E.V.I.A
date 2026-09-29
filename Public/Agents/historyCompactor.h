#pragma once

#include "Core/conversationContext.h"
#include "Core/messageRouter.h"
#include "Memory/observationLog.h"

#include <stop_token>
#include <string>
#include <vector>

namespace revia::agents
{

// What the observer drew from the turns leaving the window.
struct HistoryCompactionResult
{
    bool succeeded = false;
    std::vector<revia::memory::Observation> observations;
    std::string reason;
    double elapsedMilliseconds = 0.0;
};

// What the reflector decided to merge.
struct HistoryReflectionResult
{
    bool succeeded = false;
    std::vector<conversationContext::Merge> merges;
    std::string reason;
    double elapsedMilliseconds = 0.0;
};

// Keeps the record of a conversation's earlier part.
//
// Two passes, both on the Main model at background priority after a reply has been
// delivered, so a person never waits on either: a message of theirs preempts the pass,
// and it is tried again after that turn.
//
// The observer turns the turns leaving the window into dated observations and only
// ever adds to the log. The reflector, once the log is long, merges observations that
// overlap into one that supersedes them. Neither rewrites what is already there: a
// summary that a small model rewrites every time it grows loses detail with each pass.
class HistoryCompactor
{
public:
    [[nodiscard]] HistoryCompactionResult Observe(
        const messageRouter& router,
        const conversationContext::CompactionJob& job,
        std::stop_token stopToken = {}) const;

    [[nodiscard]] HistoryReflectionResult Reflect(
        const messageRouter& router,
        const conversationContext::ReflectionJob& job,
        std::stop_token stopToken = {}) const;

    // The instructions and grammars, shared with the model calls so they cannot drift.
    [[nodiscard]] static const char* ObserverPrompt();
    [[nodiscard]] static const char* ObserverSchema();
    [[nodiscard]] static const char* ReflectorPrompt();
    [[nodiscard]] static const char* ReflectorSchema();

    // The bounded material handed to each pass, exposed so it can be tested without a
    // model.
    [[nodiscard]] static std::string BuildObserverEnvelope(
        const conversationContext::CompactionJob& job);
    [[nodiscard]] static std::string BuildReflectorEnvelope(
        const conversationContext::ReflectionJob& job);
    [[nodiscard]] static HistoryCompactionResult ParseObservations(const std::string& raw);
    [[nodiscard]] static HistoryReflectionResult ParseMerges(const std::string& raw);
};

} // namespace revia::agents
