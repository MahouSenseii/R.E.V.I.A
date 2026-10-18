#include "Agents/responseFilterSettings.h"
#include "Core/conversationMessage.h"
#include "LLM/responseTypes.h"
#include "Agents/conversationAgent.h"
#include "Agents/replyFormat.h"
#include "Agents/replyStreamGate.h"

#include <chrono>
#include <sstream>

namespace revia::agents
{

responseOutput ConversationAgent::Execute(const messageRouter& router, const std::string& input,
    const std::vector<conversationMessage>& context, const responseFilterSettings& filterSettings,
    const ResponseFilterContext& filterContext, const std::stop_token stopToken, messageRouter::DeltaHandler onDelta,
    const revia::intelligence::IntelligenceDecision& decision, const llm::PrivateMemoryAccess memoryAccess) const
{
    const auto replyContract = RequestedReplyContract(input);
    // Backend deltas are untrusted until the deterministic filter sequence has accepted
    // them. When nothing still to come can change an accepted sentence -- no AI reviewer
    // waits to read the whole reply, and no explicit shape must be validated complete --
    // each one is released as soon as the filters leave the reply so far unchanged.
    // Otherwise the backend still streams, for cancellation, and delivery happens once.
    const bool releaseWhileGenerating =
        onDelta && !filterSettings.bAiReviewEnabled && replyContract.rootKind == ReplyFormat::Conversation;
    ReplyStreamGate gate([&](const std::string& reply) -> std::optional<std::string>
        {
            const HardFilterResult hard = responseFilter.ApplyHard(
                input, stylePolicy.RefineReply(input, context, reply), filterContext, filterSettings.maxReplyCharacters);
            if (hard.blocked)
                return std::nullopt;
            return hard.text;
        });
    messageRouter::DeltaHandler backendDelta;
    if (releaseWhileGenerating)
    {
        backendDelta = [&](const std::string& delta)
        {
            if (stopToken.stop_requested())
                return;
            if (const std::string approved = gate.Consume(delta); !approved.empty())
                onDelta(approved);
        };
    }
    else if (onDelta)
    {
        backendDelta = [](const std::string&) {};
    }
    responseOutput output = router.RouteMessage(input, context, stopToken, backendDelta, decision, memoryAccess);
    output.bWasStreamed = !gate.Released().empty();
    if (output.bSuccess)
    {
        output.rawResponse = output.response;
        output.response = stylePolicy.RefineReply(input, context, output.response);

        const auto hardStarted = std::chrono::steady_clock::now();
        const HardFilterResult firstHard =
            responseFilter.ApplyHard(input, output.response, filterContext, filterSettings.maxReplyCharacters);
        output.response = firstHard.text;
        output.bHardFilterChanged = firstHard.changed;
        output.bHardFilterBlocked = firstHard.blocked;
        bool finalSafetyReply = firstHard.blocked;
        output.timings.push_back(
            {"response_filter_hard", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hardStarted).count()});

        std::ostringstream summary;
        summary << "Hard filter: " << (firstHard.changed ? "repaired" : "passed") << ".";
        if (filterSettings.bAiReviewEnabled && !stopToken.stop_requested())
        {
            const auto reviewStarted = std::chrono::steady_clock::now();
            const responseOutput review = router.ReviewConversationReply(input, output.response, filterContext.Describe(),
                filterSettings.aiMaxReviewTokens, stopToken,
                ConversationStylePolicy::BuildReviewEvidence(
                    input, context, filterContext.answerObligation, filterContext.privateHistoryExcluded));
            output.timings.push_back({"response_filter_ai",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - reviewStarted).count()});
            if (review.bSuccess)
            {
                const AiFilterDecision decision = responseFilter.ParseAiDecision(review.response);
                output.bAiFilterReviewed = decision.parsed;
                if (decision.parsed)
                {
                    if (decision.replace)
                    {
                        output.response = stylePolicy.RefineReply(input, context, decision.replacement);
                        output.bAiFilterChanged = true;
                    }
                    summary << " AI review: " << (decision.replace ? "replaced" : "allowed") << ".";
                    if (!decision.reason.empty())
                        summary << ' ' << decision.reason;
                }
                else
                {
                    summary << " AI review: degraded (" << decision.reason << "); hard-filtered reply retained.";
                }
            }
            else
            {
                summary << " AI review: unavailable (" << review.reason << "); hard-filtered reply retained.";
            }

            // Model-produced replacement text is untrusted too. Nothing bypasses the
            // deterministic layer, including the AI filter itself.
            const HardFilterResult finalHard =
                responseFilter.ApplyHard(input, output.response, filterContext, filterSettings.maxReplyCharacters);
            output.response = finalHard.text;
            output.bHardFilterChanged = output.bHardFilterChanged || finalHard.changed;
            output.bHardFilterBlocked = output.bHardFilterBlocked || finalHard.blocked;
            finalSafetyReply = finalHard.blocked || (firstHard.blocked && finalHard.text == firstHard.text);
        }
        else
        {
            summary << " AI review: off.";
        }
        output.filterSummary = summary.str();
        // A safety replacement has its own authority and must remain a visible refusal.
        // All other final text, including reviewer replacements, must satisfy the turn.
        if (!finalSafetyReply && !MatchesReplyContract(output.response, replyContract))
        {
            output.bSuccess = false;
            output.bShouldRemember = false;
            output.reason = "Final reply violated the explicit turn-local reply contract.";
            output.filterSummary += " Final structured validation: rejected.";
            return output;
        }
        if (onDelta && !stopToken.stop_requested())
        {
            // What was released is already the start of the reply. Only the rest follows,
            // and only when the final reply still begins with it; when the full pass
            // changed released text, the caller sees the final reply and reconciles.
            const std::string& released = gate.Released();
            if (released.empty())
                onDelta(output.response);
            else if (output.response.size() > released.size() && output.response.compare(0, released.size(), released) == 0)
                onDelta(output.response.substr(released.size()));
            output.bWasStreamed = true;
        }
    }
    return output;
}

} // namespace revia::agents
