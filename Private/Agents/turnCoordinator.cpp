#include "Agents/turnCoordinator.h"

#include <utility>

namespace revia::agents
{

TurnAgentResult TurnCoordinator::Execute(
    const messageRouter& router,
    const std::string& input,
    const std::vector<conversationMessage>& context,
    const responseFilterSettings& filterSettings,
    const ResponseFilterContext& filterContext,
    const bool evaluateMemory,
    const std::uint64_t turnId,
    const std::stop_token stopToken,
    messageRouter::DeltaHandler onDelta,
    const revia::intelligence::IntelligenceDecision& decision) const
{
    TurnAgentResult result;
    result.response =
        conversationAgent.Execute(
            router, input, context, filterSettings, filterContext, stopToken,
            std::move(onDelta), decision);

    // The interactive reply owns inference priority. Starting memory classification
    // first can contend with chat and embedding work on the same GPU.
    if (evaluateMemory && result.response.bSuccess && !stopToken.stop_requested())
    {
        memoryAgent.Submit(router, input, result.response.response, turnId);
        result.memoryQueued = true;
    }
    return result;
}

std::vector<MemoryAgentEvent> TurnCoordinator::DrainMemoryEvents()
{
    return memoryAgent.DrainEvents();
}

LearnedFindingResult TurnCoordinator::SubmitLearnedFinding(
    const messageRouter& router,
    memoryDecision decision,
    const std::uint64_t turnId)
{
    return memoryAgent.SubmitLearnedFinding(router, std::move(decision), turnId);
}

void TurnCoordinator::BackfillMemoryEmbeddings(
    const messageRouter& router,
    const std::string& embeddingModel)
{
    memoryAgent.SubmitEmbeddingBackfill(router, embeddingModel);
}

void TurnCoordinator::Stop()
{
    memoryAgent.Stop();
}

} // namespace revia::agents
