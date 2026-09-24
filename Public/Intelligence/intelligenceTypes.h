#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace revia::intelligence
{

enum class IntelligenceTier
{
    Reflex,
    Fast,
    Main,
    Expert,
    Vision,
    ExpertVision
};

enum class ReasoningMode
{
    Fast,
    Deep
};

// Everything the router is allowed to decide on, and nothing it cannot be told.
//
// Every field here has a producer in ConversationRuntime::BuildRoutingContext, which is
// the only place a context is assembled for a real turn. That is a rule rather than an
// observation: a field that changes the selected tier but that nothing ever sets is not
// a feature waiting to be used, it is a branch no request can reach, and it makes the
// router's tests describe behaviour the runtime does not have.
//
// Two such fields were removed rather than given invented producers:
//
//   suppliedFileCount -- there is no attachment boundary in this application. Nothing
//   supplies files to a conversational turn: the chat input is text, file reads go
//   through the action path and come back as a result rather than as context, and
//   /show puts a picture on the canvas rather than into the prompt. There was no
//   authoritative origin to connect it to.
//
//   toolUseRequested -- a resolved tool or action request never reaches the router.
//   ReviaSession::TryHandleCommand dispatches those turns and returns before any
//   generation happens, so by the time a context is built the answer is always "no".
//   Setting it from a keyword guess would have been a different signal wearing the same
//   name.
struct RoutingContext
{
    bool visionRequired = false;
    bool expertVisionPreferred = false;
    bool explicitResearch = false;
    // Whether the previous delivered answer in this conversation is one the runtime has
    // its own reason to distrust: the generation failed, the deterministic filter had to
    // replace the reply, or the quality monitor found it ungrounded. Observed by the
    // runtime after delivery, never asked of the model -- a model's opinion of its own
    // last answer is not evidence about it.
    bool previousUncertainty = false;
    std::size_t recentContextCharacters = 0;
    // The tier that produced the previous successfully delivered conversational answer
    // in this conversation, so a short follow-up that only makes sense against that
    // answer ("Why?", "Explain that.") keeps the effort the answer was worth instead of
    // collapsing to the cheapest brain because it is three characters long.
    //
    // Empty whenever there is no trustworthy previous answer to inherit from: a first
    // turn, a cancelled or failed generation, a restored conversation with no recorded
    // tier, or an evaluation run that supplies its own corpus history. Empty means fall
    // back conservatively; it never means Fast.
    //
    // This is transient routing context. It is effort, not identity: it is never
    // persisted, never enters the identity packet, memory, relationships, or
    // development, and it does not change who Revia is.
    std::optional<IntelligenceTier> previousAssistantTier;
};

struct IntelligenceDecision
{
    IntelligenceTier requestedTier = IntelligenceTier::Main;
    IntelligenceTier selectedTier = IntelligenceTier::Main;
    ReasoningMode mode = ReasoningMode::Fast;
    std::string selectedModel;
    std::string reason;
    float confidence = 0.0F;
    bool fallbackUsed = false;
    std::string fallbackReason;
};

[[nodiscard]] std::string ToString(IntelligenceTier tier);
[[nodiscard]] std::string ToString(ReasoningMode mode);

} // namespace revia::intelligence
