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

// A nomination, not permission. The curiosity layer may suggest silence, a line of
// conversation, or a read-only research query. It cannot execute the suggestion and it
// cannot decide whether interrupting the user is welcome.
enum class CuriosityAction
{
    Silence,
    Speak,
    Research
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
        std::stop_token stopToken = {}) const;

    // Public so parser and prompt contracts can be tested without a running model.
    [[nodiscard]] static CuriosityDecision ParseDecision(const std::string& rawDecision);
    [[nodiscard]] static std::string BuildContextPrompt(
        const std::vector<conversationMessage>& recentConversation,
        const runtime::AffectSnapshot& affect,
        const std::string& desktopContext = {});
};

} // namespace revia::agents
