#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace revia::agents
{

// When Revia may hand a request from chat to her Agent Studio workers. She always stays
// responsible for the conversation and the final answer; this only decides whether
// delegation is one of the abilities she can reach for on a given turn.
enum class DelegationMode
{
    // No delegation tools at all.
    Off,
    // Only when the person asks for it in their own words ("use your agents to check this").
    OnRequest,
    // She may also decide on her own that a request deserves separate investigation.
    Auto
};

// "off", "on_request" or "auto", the spelling conversation.delegation uses.
[[nodiscard]] std::string ToString(DelegationMode mode);
// Exact spellings only; anything else is Off, so an unreadable value never widens what
// she may do. Settings validation refuses unknown spellings before this is reached.
[[nodiscard]] DelegationMode DelegationModeFromString(const std::string& value);

// What the person's own words this turn say about delegating. Read deterministically from
// the input, so no extra model call classifies every message.
enum class DelegationCue
{
    None,
    // "Use your agents to check this", "delegate this", "have your workers look into it".
    UseAgents,
    // "Answer this directly", "don't use your agents". Wins over UseAgents, because the
    // negative phrasing contains the positive one.
    AnswerDirectly
};

[[nodiscard]] DelegationCue ReadDelegationCue(const std::string& input);

// Whether delegate_task is offered this turn. Off never offers it, an explicit "answer
// directly" never does, On request needs the person's request, and Auto leaves the choice
// to the model.
[[nodiscard]] bool OffersDelegation(DelegationMode mode, DelegationCue cue);

// One delegate_task call as the session receives it. The objective and reason are the
// model's words, already bounded by the tool's schema; the rest is bound by the runtime
// from the turn that made the call, never written by the model.
struct DelegationSubmission
{
    std::string objective;
    std::string reason;
    // The person's words on the turn that delegated, so the eventual report answers what
    // they actually asked rather than the model's paraphrase of it.
    std::string userRequest;
    std::uint64_t originTurnId = 0;
    std::uint64_t audienceRevision = 0;
};

inline constexpr std::size_t MaximumDelegatedObjectiveCharacters = 1000;
inline constexpr std::size_t MaximumDelegationReasonCharacters = 300;

} // namespace revia::agents
