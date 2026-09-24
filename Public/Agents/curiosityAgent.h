#pragma once

#include "Library/structLibrary.h"
#include "Runtime/affectTypes.h"

#include <cstddef>
#include <stop_token>
#include <string>
#include <vector>

class messageRouter;

namespace revia::agents
{

// A nomination, not permission. Idle choices include private work and bounded PC
// actions as well as research and conversation. The runtime owns execution and
// decides whether an interruption is welcome.
enum class CuriosityAction
{
    Silence,
    Speak,
    Research,
    Think,
    Observe,
    Create,
    Computer
};

[[nodiscard]] std::string ToString(CuriosityAction action);

struct CuriosityDecision
{
    bool valid = false;
    CuriosityAction action = CuriosityAction::Silence;
    std::string topic;
    std::string query;
    std::string rationale;
    float confidence = 0.0F;
    std::string error;
};

struct IdleActivityContext
{
    long long quietSeconds = 0;
    float boredom = 0.0F;
    float socialNeed = 0.0F;
    // Someone touched the keyboard or mouse in the last few minutes, so a remark has
    // somebody to land on. Being at the computer is when talking works, not a reason
    // to stay out of the way.
    bool userAtComputer = false;
    bool userInFullScreen = false;
    // Whether anything said now could actually be heard: spontaneous speech is on and
    // the attention policy has no cooldown or budget in the way.
    bool speakAllowed = false;
    bool researchAllowed = false;
    bool observationAllowed = false;
    bool computerAllowed = false;
    int unansweredOpenings = 0;
    std::string recentActivities;
    // Topics nominated recently that the runtime could not act on. Without this the
    // planner, which cannot see why a nomination went nowhere, picks the same one again.
    std::string setAside;
    std::string computerScope;
};

// Produces one tightly bounded structured nomination from recent dialogue and Revia's
// current affect. The caller still owns attention policy, capability checks, execution,
// telemetry, and whether anything is shown or spoken.
class CuriosityAgent
{
public:
    static constexpr std::size_t MaximumConversationMessages = 8;
    static constexpr std::size_t MaximumConversationCharacters = 3600;
    static constexpr std::size_t MaximumMessageCharacters = 700;
    static constexpr std::size_t MaximumDesktopContextCharacters = 2400;
    static constexpr std::size_t MaximumPromptCharacters = 10000;
    static constexpr std::size_t MaximumTopicCharacters = 160;
    static constexpr std::size_t MaximumQueryCharacters = 320;
    static constexpr std::size_t MaximumRationaleCharacters = 400;

    [[nodiscard]] CuriosityDecision Nominate(
        const messageRouter& router,
        const std::vector<conversationMessage>& recentConversation,
        const runtime::AffectSnapshot& affect,
        const std::string& desktopContext,
        std::stop_token stopToken = {},
        const IdleActivityContext& idle = {}) const;

    // The only choices offered to the planner, and enforced by its reply schema. Telling
    // a small model that research is unavailable did not stop it choosing research
    // twelve times in six minutes, each choice discarded after a full planning call.
    [[nodiscard]] static std::vector<std::string> AvailableActions(
        const IdleActivityContext& idle);

    // Public so parser and prompt contracts can be tested without a running model.
    [[nodiscard]] static CuriosityDecision ParseDecision(const std::string& rawDecision);
    [[nodiscard]] static std::string BuildContextPrompt(
        const std::vector<conversationMessage>& recentConversation,
        const runtime::AffectSnapshot& affect,
        const std::string& desktopContext = {},
        const IdleActivityContext& idle = {});
};

} // namespace revia::agents
