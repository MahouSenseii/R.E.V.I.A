#pragma once

#include "Core/messageRouter.h"
#include "Intelligence/intelligenceTypes.h"
#include "Library/structLibrary.h"

#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::agents
{

// One deliberation Revia ran on herself before answering.
//
// Not a second opinion and not a reviewer. The questions are hers, written in her own
// voice, and she is told plainly that she is the one asking them -- a turn where an
// unattributed voice interrogates her would read as two people sharing a name, which is
// the one thing the architecture will not have.
struct SelfInquiryResult
{
    bool ran = false;
    // What she put to herself, first person, in the order she thought of it.
    std::vector<std::string> questions;
    // What she got as far as settling. Empty when she did not get that far, which is a
    // legitimate outcome and must not be filled in with a draft of the reply.
    std::string settled;
    // Why the gate opened, or why nothing ran. Always populated.
    std::string reason;
    double elapsedMilliseconds = 0.0;

    [[nodiscard]] bool HasQuestions() const { return ran && !questions.empty(); }
    // Appended to the posture of the turn that follows, so the thinking feeds the answer
    // instead of being spent on the transcript. Empty when nothing ran.
    [[nodiscard]] std::string PromptBlock() const;
    // What the shell shows in chat. Empty when nothing ran.
    [[nodiscard]] std::string TranscriptBlock() const;
};

struct SelfInquiryDecision
{
    bool shouldThink = false;
    std::string reason;
};

// Bounds on how often Revia is allowed to stop and think out loud.
//
// Every value exists to stop deliberation becoming a verbal tic. Thinking that happens
// on every turn is not thinking, it is a preamble, and a user who sees it constantly
// stops reading it -- which defeats the point of showing it at all.
struct SelfInquiryLimits
{
    bool enabled = true;
    // Turns that must pass after one inquiry before another may run.
    std::size_t cooldownTurns = 3;
    // Below this, the turn is too small to be a major problem regardless of what the
    // router said. "why" routes to Main; it is not a problem worth deliberating over.
    std::size_t minimumInputCharacters = 40;
    std::size_t maximumQuestions = 4;
};

// Decides whether this turn is a major enough problem to be worth stopping over.
//
// Deterministic and modelless on purpose: whether Revia thinks must not itself depend on
// a model call, or a turn costs two inferences before the first token of the answer. The
// signal it reads is the one the intelligence router already computed, because a second
// difficulty classifier would eventually disagree with the first about the same message.
class SelfInquiryPolicy
{
public:
    SelfInquiryPolicy() = default;
    explicit SelfInquiryPolicy(SelfInquiryLimits inputLimits) : limits(inputLimits) {}

    [[nodiscard]] SelfInquiryDecision Consider(
        const std::string& input,
        const intelligence::IntelligenceDecision& routing,
        bool proactive,
        std::uint64_t turnId) const;

    // A completed inquiry is the deep-reasoning pass for this turn. The final model
    // still uses the originally selected tier, but it must spend its output allowance
    // on the visible answer instead of opening a second hidden thinking block.
    [[nodiscard]] static intelligence::IntelligenceDecision FinalAnswerRouting(
        const intelligence::IntelligenceDecision& routing,
        bool inquiryCompleted);

    // Recorded only when an inquiry actually produced questions. A failed or empty pass
    // must not start the cooldown, or one unreachable model silences her for three turns.
    void RecordInquiry(std::uint64_t turnId);

    [[nodiscard]] const SelfInquiryLimits& Limits() const { return limits; }
    void SetLimits(SelfInquiryLimits inputLimits) { limits = inputLimits; }

private:
    SelfInquiryLimits limits;
    std::uint64_t lastInquiryTurn = 0;
    bool hasRun = false;
};

// Runs the bounded model call and turns what comes back into her questions.
//
// It never produces the visible reply. One conversational model still generates the
// final turn; this hands that turn a block of her own thinking to answer from.
class SelfInquiryAgent
{
public:
    [[nodiscard]] SelfInquiryResult Ask(
        const messageRouter& router,
        const std::string& input,
        // The same posture the final turn is generated under, bounded. Passing the
        // identical description of this moment is what keeps the thinking hers rather
        // than a generic reasoner's.
        const std::string& identityPosture,
        const std::vector<conversationMessage>& context,
        std::size_t maximumQuestions,
        std::stop_token stopToken = {}) const;

    // The parse, exposed so the schema can be tested without a model or a socket.
    [[nodiscard]] static SelfInquiryResult Parse(
        const std::string& rawInquiry,
        std::size_t maximumQuestions);

    // The bounded envelope handed to the model, exposed for the same reason.
    [[nodiscard]] static std::string BuildEnvelope(
        const std::string& input,
        const std::string& identityPosture,
        const std::vector<conversationMessage>& context);
};

} // namespace revia::agents
