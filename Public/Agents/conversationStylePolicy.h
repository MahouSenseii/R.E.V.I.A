#pragma once

#include "Library/structLibrary.h"

#include <string>
#include <vector>

namespace revia::agents
{

// Keeps conversational presentation grounded and varied without owning inference.
//
// The profile defines who Revia is. This policy supplies only turn-local guidance and
// removes a small, explicit set of stock follow-up questions that make otherwise good
// replies sound like a help-desk script. It never rewrites factual answer content.
class ConversationStylePolicy
{
public:
    [[nodiscard]] std::string BuildTurnGuidance(
        const std::string& input,
        const std::vector<conversationMessage>& context) const;

    [[nodiscard]] std::string RefineReply(
        const std::string& input,
        const std::vector<conversationMessage>& context,
        const std::string& reply) const;
    [[nodiscard]] bool IsGenericContinuation(const std::string& sentence) const;
    [[nodiscard]] bool ShouldSuppressSpokenFragment(
        const std::string& input,
        const std::vector<conversationMessage>& context,
        const std::string& fragment,
        bool alreadySpokeFragment) const;
    [[nodiscard]] bool CanStreamReply(const std::string& input) const;

private:
    [[nodiscard]] static bool LooksLikeCorrection(const std::string& input);
    [[nodiscard]] static bool LooksLikeBriefAcknowledgement(const std::string& input);
    [[nodiscard]] static bool LooksLikePreferenceStatement(const std::string& input);
    [[nodiscard]] static bool LooksLikeMotiveQuestion(const std::string& input);
    [[nodiscard]] static bool LooksLikeWellbeingQuestion(const std::string& input);
    [[nodiscard]] static bool LooksLikeSocialGreeting(const std::string& input);
    [[nodiscard]] static bool LooksLikeEmotionQuestion(const std::string& input);
    [[nodiscard]] static bool ContainsUnsupportedOperationalClaim(const std::string& reply);
    [[nodiscard]] static bool ContainsClaimedPreferenceAction(const std::string& reply);
    [[nodiscard]] static bool HasExplicitReason(const std::vector<conversationMessage>& context);
    [[nodiscard]] static bool ExpressesUncertainty(const std::string& reply);
    [[nodiscard]] static bool SpeculatesAboutMotive(const std::string& reply);
};

} // namespace revia::agents
