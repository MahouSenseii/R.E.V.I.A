#include "Agents/conversationAgent.h"

#include <chrono>
#include <sstream>

namespace revia::agents
{

responseOutput ConversationAgent::Execute(
    const messageRouter& router,
    const std::string& input,
    const std::vector<conversationMessage>& context,
    const responseFilterSettings& filterSettings,
    const ResponseFilterContext& filterContext,
    const std::stop_token stopToken,
    messageRouter::DeltaHandler onDelta,
    const revia::intelligence::IntelligenceDecision& decision) const
{
    responseOutput output =
        router.RouteMessage(input, context, stopToken, std::move(onDelta), decision);
    if (output.bSuccess)
    {
        output.rawResponse = output.response;
        output.response = stylePolicy.RefineReply(input, context, output.response);

        const auto hardStarted = std::chrono::steady_clock::now();
        const HardFilterResult firstHard = responseFilter.ApplyHard(
            input, output.response, filterContext, filterSettings.maxReplyCharacters);
        output.response = firstHard.text;
        output.bHardFilterChanged = firstHard.changed;
        output.timings.push_back({
            "response_filter_hard",
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - hardStarted).count()});

        std::ostringstream summary;
        summary << "Hard filter: " << (firstHard.changed ? "repaired" : "passed")
            << ".";
        if (filterSettings.bAiReviewEnabled && !stopToken.stop_requested())
        {
            const auto reviewStarted = std::chrono::steady_clock::now();
            const responseOutput review = router.ReviewConversationReply(
                input,
                output.response,
                filterContext.Describe(),
                filterSettings.aiMaxReviewTokens,
                stopToken);
            output.timings.push_back({
                "response_filter_ai",
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - reviewStarted).count()});
            if (review.bSuccess)
            {
                const AiFilterDecision decision =
                    responseFilter.ParseAiDecision(review.response);
                output.bAiFilterReviewed = decision.parsed;
                if (decision.parsed)
                {
                    if (decision.replace)
                    {
                        output.response = stylePolicy.RefineReply(
                            input, context, decision.replacement);
                        output.bAiFilterChanged = true;
                    }
                    summary << " AI review: "
                        << (decision.replace ? "replaced" : "allowed") << ".";
                    if (!decision.reason.empty()) summary << ' ' << decision.reason;
                }
                else
                {
                    summary << " AI review: degraded (" << decision.reason
                        << "); hard-filtered reply retained.";
                }
            }
            else
            {
                summary << " AI review: unavailable (" << review.reason
                    << "); hard-filtered reply retained.";
            }

            // Model-produced replacement text is untrusted too. Nothing bypasses the
            // deterministic layer, including the AI filter itself.
            const HardFilterResult finalHard = responseFilter.ApplyHard(
                input, output.response, filterContext, filterSettings.maxReplyCharacters);
            output.response = finalHard.text;
            output.bHardFilterChanged = output.bHardFilterChanged || finalHard.changed;
        }
        else
        {
            summary << " AI review: off.";
        }
        output.filterSummary = summary.str();
    }
    return output;
}

} // namespace revia::agents
