#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace revia::agents
{

struct ConversationQualitySnapshot
{
    std::size_t turns = 0;
    std::size_t passingTurns = 0;
    std::size_t groundednessFlags = 0;
    std::size_t stockTailFlags = 0;
    std::size_t repetitionFlags = 0;
    std::size_t ownershipFlags = 0;
    std::vector<std::string> lastFlags;

    [[nodiscard]] std::string Summary() const;
};

// Lightweight runtime regression monitor. It does not rewrite replies and does not call
// a model; it reports known conversation-quality failures to the Pipelines tab and logs.
class ConversationQualityMonitor
{
public:
    [[nodiscard]] ConversationQualitySnapshot Observe(
        const std::string& userInput,
        const std::string& response);
    [[nodiscard]] ConversationQualitySnapshot Snapshot() const;

private:
    mutable std::mutex mutex;
    ConversationQualitySnapshot snapshot;
    std::deque<std::string> recentOpenings;
};

} // namespace revia::agents
