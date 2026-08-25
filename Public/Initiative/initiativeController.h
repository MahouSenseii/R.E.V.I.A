#pragma once

#include "Goals/goalTypes.h"
#include "Initiative/attentionPolicy.h"
#include "Initiative/conversationStarter.h"
#include "Perception/activityHistory.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace revia::initiative
{

// Something Revia would like to say, before it has been allowed to say it.
//
// A proposal executes nothing. It carries the evidence that prompted it so the user can
// judge the reasoning rather than just the conclusion, and so a wrong one is diagnosable
// after the fact instead of only irritating.
struct Proposal
{
    enum class Kind
    {
        ActionOffer,
        ConversationStarter
    };

    std::string id;
    std::string message;
    std::string evidence;
    float confidence = 0.0f;
    // Set when accepting this proposal should hand a request to the goal runner. Empty
    // for a proposal that is only an observation worth mentioning.
    std::string goalRequest;
    // Set when accepting should resume a goal an earlier run left unfinished. Resuming
    // adds no authority: the runner re-verifies every remaining step exactly as it would
    // for a typed /goals resume.
    std::string resumeGoalId;
    std::chrono::system_clock::time_point createdAt = std::chrono::system_clock::now();
    Kind kind = Kind::ActionOffer;
};

enum class ProposalOutcome
{
    Pending,
    Accepted,
    Dismissed,
    Expired
};

// Forms proposals from Tier 0 evidence and asks the attention policy whether now is a
// moment to offer one.
//
// The model is not consulted about whether to interrupt. It supplies content and a
// confidence; the decision belongs to AttentionPolicy, for the same reason a model does
// not choose its own capability scope.
class InitiativeController
{
public:
    InitiativeController() = default;

    void Configure(initiativeSettings settings);
    void UpdateSettings(initiativeSettings settings);

    // Considers the recent session and returns a proposal when one clears the gate.
    // Returns no proposal and records the reason otherwise.
    struct Consideration
    {
        bool hasProposal = false;
        Proposal proposal;
        AttentionVerdict verdict = AttentionVerdict::Disabled;
    };
    // Everything Revia currently knows that could justify speaking first. Passed as one
    // struct because the set of evidence sources is expected to grow, and every new one
    // should be weighed against the others rather than bolted on as another entry point.
    struct Evidence
    {
        std::vector<perception::ActivitySpan> recentActivity;
        // Goals an earlier run left unfinished. Concrete and actionable, so they outrank
        // an observation about how the session has been spent.
        std::vector<goals::Goal> unfinishedGoals;
        // Event-derived reasons to begin ordinary conversation. These are not timers and
        // not action requests; a natural user reply continues the normal dialogue.
        std::vector<StarterCue> conversationCues;
    };

    [[nodiscard]] Consideration Consider(
        const Evidence& evidence,
        const AttentionContext& context);

    // Commits a proposal only after its output or private research actually succeeded.
    // Consider() merely reserves admission, so cancellation and generation failures do
    // not consume cooldown or the hourly budget.
    [[nodiscard]] bool Commit(
        const std::string& proposalId,
        std::chrono::system_clock::time_point when);
    void Accept(const std::string& proposalId);
    // A normal reply accepts a conversational opening; a natural refusal such as "not
    // now" dismisses it. Neither path requires a slash command.
    void RecordConversationResponse(
        const std::string& response,
        std::chrono::system_clock::time_point when);
    void Dismiss(const std::string& proposalId, std::chrono::system_clock::time_point when);
    void Expire(const std::string& proposalId);

    [[nodiscard]] std::vector<Proposal> Pending() const;
    [[nodiscard]] InitiativeCounters Counters() const;
    [[nodiscard]] float Precision() const;
    [[nodiscard]] bool IsRateReduced() const;
    [[nodiscard]] std::string Status() const;

    // Exposed so the same wording can be asserted in tests without a live desktop.
    // Picks the strongest available proposal, or none. Static so the choice between
    // evidence sources can be tested without a live desktop or a running session.
    [[nodiscard]] static bool BuildProposal(
        const Evidence& evidence,
        Proposal& outProposal);
    [[nodiscard]] static bool BuildActivityProposal(
        const std::vector<perception::ActivitySpan>& recent,
        Proposal& outProposal);
    [[nodiscard]] static bool BuildUnfinishedGoalProposal(
        const std::vector<goals::Goal>& unfinishedGoals,
        Proposal& outProposal);
    [[nodiscard]] static bool BuildConversationProposal(
        const std::vector<StarterCue>& cues,
        Proposal& outProposal);

private:
    mutable std::mutex mutex;
    initiativeSettings configuration;
    AttentionPolicy policy;
    std::vector<std::pair<Proposal, ProposalOutcome>> proposals;
    std::unordered_set<std::string> committedProposals;
    std::string lastSubject;
    std::uint64_t nextId = 1;
};

} // namespace revia::initiative
