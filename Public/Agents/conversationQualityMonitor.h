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

    // The individual signals behind the counters above, exposed as pure functions.
    //
    // The evaluation corpus scores a reply with these rather than keeping its own copy of
    // the phrase lists. Two lists that are supposed to mean the same thing eventually
    // disagree, and a suite that disagrees with the live counters is worse than no suite:
    // it reports a regression the runtime does not see, or misses one it does.
    [[nodiscard]] static bool ClaimsInventedPhysicalLife(const std::string& response);
    [[nodiscard]] static bool EndsWithStockTail(const std::string& response);
    [[nodiscard]] static bool ProjectsStateOntoUser(
        const std::string& userInput,
        const std::string& response);
    // The lowered first clause, which is what "repeated opening" is measured against.
    [[nodiscard]] static std::string OpeningOf(const std::string& response);

private:
    mutable std::mutex mutex;
    ConversationQualitySnapshot snapshot;
    std::deque<std::string> recentOpenings;
};

} // namespace revia::agents
