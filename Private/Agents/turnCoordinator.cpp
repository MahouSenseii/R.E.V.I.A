#include "Agents/responseFilterSettings.h"
#include "Core/conversationMessage.h"
#include "Memory/memoryTypes.h"
#include "Agents/turnCoordinator.h"

#include <utility>

namespace revia::agents
{

TurnCoordinator::TurnCoordinator() = default;

TurnCoordinator::TurnCoordinator(std::string memoryDatabasePath) : memoryAgent(std::move(memoryDatabasePath))
{
}

void TurnCoordinator::SetAdmissionGuard(std::function<bool()> guard)
{
    memoryAgent.SetAdmissionGuard(std::move(guard));
}

TurnAgentResult TurnCoordinator::Execute(const messageRouter& router, const std::string& input,
    const std::vector<conversationMessage>& context, const responseFilterSettings& filterSettings,
    const ResponseFilterContext& filterContext, const bool evaluateMemory, const ResponseProvenance provenance, const std::uint64_t turnId,
    const std::stop_token stopToken, messageRouter::DeltaHandler onDelta, const revia::intelligence::IntelligenceDecision& decision,
    const llm::PrivateMemoryAccess memoryAccess, std::function<bool()> contextAdmission) const
{
    TurnAgentResult result;
    const auto admitted = [&contextAdmission]()
    {
        try { return !contextAdmission || contextAdmission(); }
        catch (...) { return false; }
    };
    if (!admitted())
    {
        return result;
    }
    auto guardedDelta = [contextAdmission, onDelta = std::move(onDelta)](const std::string& text)
    {
        bool current = false;
        try { current = !contextAdmission || contextAdmission(); }
        catch (...) { return; }
        if (current && onDelta)
        {
            onDelta(text);
        }
    };
    result.response =
        conversationAgent.Execute(
            router, input, context, filterSettings, filterContext, stopToken,
            std::move(guardedDelta), decision, memoryAccess);

    if (!admitted())
    {
        result.response = {};
        return result;
    }

    // The interactive reply owns inference priority. Starting memory classification
    // first can contend with chat and embedding work on the same GPU.
    if (memoryAccess == llm::PrivateMemoryAccess::ProfileSetting &&
        evaluateMemory && result.response.bSuccess && !stopToken.stop_requested())
    {
        memoryAgent.Submit(
            router, input, result.response.response, provenance, turnId, contextAdmission);
        result.memoryQueued = true;
    }
    return result;
}

std::vector<MemoryAgentEvent> TurnCoordinator::DrainMemoryEvents()
{
    return memoryAgent.DrainEvents();
}

LearnedFindingResult TurnCoordinator::SubmitLearnedFinding(const messageRouter& router, memoryDecision decision, const std::uint64_t turnId)
{
    return memoryAgent.SubmitLearnedFinding(router, std::move(decision), turnId);
}

LearnedFindingResult TurnCoordinator::SubmitLearnedFinding(const messageRouter& router, memoryDecision decision,
    const std::uint64_t turnId, std::string* outMemoryId)
{
    return memoryAgent.SubmitLearnedFinding(router, std::move(decision), turnId, outMemoryId);
}

void TurnCoordinator::BackfillMemoryEmbeddings(const messageRouter& router, const std::string& embeddingModel)
{
    memoryAgent.StartEmbeddingBackfill(router, embeddingModel);
}

void TurnCoordinator::Stop()
{
    memoryAgent.Stop();
}

} // namespace revia::agents
