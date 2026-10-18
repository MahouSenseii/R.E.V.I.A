#pragma once

#include "Core/conversationMessage.h"
#include "LLM/Cloud/cloudProtocol.h"

#include <string>
#include <vector>

namespace revia::llm::cloud
{

enum class CloudUse
{
    Never,
    Ask,
    HardTurns
};

// Anything but "ask" or "hardTurns" reads as Never, so a typo cannot widen what is sent.
[[nodiscard]] CloudUse ParseCloudUse(const std::string& value);

// An explicit request to use the cloud for this turn, recognised from what the user typed:
//   /cloud ask [--screen] [--clipboard] [--memory] <question>
//   ask the cloud[,|:] <question>
// The flags are the only way to send the screen summary, the clipboard or saved memories.
// They exist only on the slash form, because a phrase in ordinary speech is too easy to
// say by accident.
struct CloudAsk
{
    bool bExplicit = false;
    bool bIncludeScreen = false;
    bool bIncludeClipboard = false;
    bool bIncludeMemories = false;
    // The input without the cloud prefix: what the model is asked.
    std::string question;
};

[[nodiscard]] CloudAsk ParseCloudAsk(const std::string& input);

struct CloudRouteInputs
{
    CloudUse use = CloudUse::Never;
    bool bPublicAudience = false;
    bool bProactive = false;
    bool bExplicitAsk = false;
    // The intelligence router chose Expert or ExpertVision for this turn.
    bool bRouterChoseExpert = false;
};

struct CloudRouteDecision
{
    bool bUseCloud = false;
    // An explicit ask is answered by the cloud or by a plain refusal, never quietly by the
    // local model: the user asked for one specific thing.
    bool bExplicit = false;
    std::string reason;
};

// Whether to try the cloud for this turn. Enabled, key and /cloud off are checked by the
// service itself on every request; this only decides which turns are candidates.
[[nodiscard]] CloudRouteDecision DecideCloudRoute(const CloudRouteInputs& inputs);

// One turn's request to the cloud, carrying the private pieces only when the matching
// per-turn flag was set.
struct CloudTurnRequest
{
    bool bExplicit = false;
    bool bPublicAudience = false;
    bool bIncludeScreen = false;
    bool bIncludeClipboard = false;
    bool bIncludeMemories = false;
    std::string screenObservation;
    std::string clipboardText;
    std::string privateMemories;
};

struct CloudPayloadResult
{
    bool bAllowed = false;
    CloudPayload payload;
    // Why the request was kept on this PC. Safe to show and to log: it names what was
    // found, never the text itself.
    std::string refusal;
    // Names of the private sections removed from the history, for the activity log.
    std::vector<std::string> stripped;
};

// Removes Revia's runtime block and every private reference section (clipboard, recalled
// memories and conversations, page text, relationship and state lines) from one message.
[[nodiscard]] std::string StripPrivateContext(const std::string& text, std::vector<std::string>* stripped = nullptr);

// Builds what may leave the PC: the persona's system prompt, the user/assistant turns with
// private sections stripped, and the opted-in pieces. Refuses a public audience, and runs
// the sensitive-content detector over the whole payload; anything it recognises keeps the
// request local.
[[nodiscard]] CloudPayloadResult BuildCloudPayload(
    const std::string& personaSystemPrompt, const std::vector<conversationMessage>& history, const CloudTurnRequest& turn);

} // namespace revia::llm::cloud
