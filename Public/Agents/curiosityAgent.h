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
    bool userBusy = false;
    bool researchAllowed = false;
    bool observationAllowed = false;
    bool computerAllowed = false;
    int unansweredOpenings = 0;
    std::string recentActivities;
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

    // Public so parser and prompt contracts can be tested without a running model.
    [[nodiscard]] static CuriosityDecision ParseDecision(const std::string& rawDecision);
    [[nodiscard]] static std::string BuildContextPrompt(
        const std::vector<conversationMessage>& recentConversation,
        const runtime::AffectSnapshot& affect,
        const std::string& desktopContext = {},
        const IdleActivityContext& idle = {});
};

} // namespace revia::agents
