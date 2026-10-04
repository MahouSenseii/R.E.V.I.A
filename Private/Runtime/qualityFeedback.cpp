#include "Runtime/reviaSession.h"

#include "Audit/contentDigest.h"
#include "Memory/sensitiveContent.h"

#include <nlohmann/json.hpp>

namespace revia::runtime
{
namespace
{
bool OwnerMutationCanceled(const std::stop_token stopToken, std::string& error)
{
    if (!stopToken.stop_requested())
    {
        return false;
    }
    error = "The owner review or revision was canceled.";
    return true;
}

nlohmann::json LlmFingerprintFields(const llmSettings& value)
{
    return {{"backend", value.backend}, {"modelName", value.modelName}, {"contextSize", value.contextSize}, {"maxTokens", value.maxTokens},
        {"autoMaxTokens", value.bAutoMaxTokens}, {"temperature", value.temperature}, {"device", value.device},
        {"splitMode", value.splitMode}, {"parallelRequests", value.parallelRequests}, {"visionEnabled", value.bVisionEnabled},
        {"stablePromptPrefix", value.bStablePromptPrefix}, {"allowPromptCache", value.bAllowPromptCache}};
}

learning::QualityFeedback CapturedFeedback(learning::QualityFeedback feedback, const aiProfile& profile, const llmSettings& main,
    const llmSettings& fast, const llmSettings& expert, const intelligenceSettings& routing)
{
    const nlohmann::json baseline = {{"id", profile.id}, {"displayName", profile.displayName}, {"description", profile.description},
        {"systemPrompt", profile.systemPrompt}, {"memoryEnabled", profile.bMemoryEnabled},
        {"temperatureOverride", profile.bHasTemperatureOverride}, {"maxTokensOverride", profile.bHasMaxTokensOverride},
        {"temperature", profile.temperature}, {"maxTokens", profile.maxTokens}, {"personalityBaseline", profile.personalityBaseline},
        {"preferences", profile.preferences}, {"answerObligation", static_cast<int>(profile.answerObligation)}};
    const nlohmann::json configuration = {{"main", LlmFingerprintFields(main)}, {"fast", LlmFingerprintFields(fast)},
        {"expert", LlmFingerprintFields(expert)}, {"routingEnabled", routing.bEnabled}, {"fastEnabled", routing.fast.bEnabled},
        {"expertEnabled", routing.expert.bEnabled}};
    feedback.dependencies["loaded-profile-sha256"] = audit::ContentDigest(baseline.dump());
    feedback.dependencies["effective-llm-config-sha256"] = audit::ContentDigest(configuration.dump());
    feedback.dependencies["native-answer-contract"] = "judged-quality-v1";
    return feedback;
}
}

bool ReviaSession::RecordQualityFeedback(const learning::QualityFeedback& feedback, std::string& taskId, std::string& recordId,
    std::string& error, const std::stop_token stopToken)
{
    taskId.clear();
    recordId.clear();
    if (OwnerMutationCanceled(stopToken, error))
    {
        return false;
    }
    {
        const std::scoped_lock lock(operationMutex, learningStudioMutex, audienceMutex);
        if (OwnerMutationCanceled(stopToken, error))
        {
            return false;
        }
        const RuntimeStamp current = Stamp();
        if (feedback.source != learning::QualityEvidenceSource::OwnerJudgment || feedback.criterionSatisfied || !Admits(feedback.origin) ||
            !feedback.origin.SameSession(current) || feedback.origin.policyVersion != current.policyVersion ||
            feedback.audienceRevision == 0 || feedback.audienceRevision != configuredAudience.revision ||
            configuredAudience.kind != identity::AudienceKind::Private || lastInputAudience != identity::AudienceKind::Private)
        {
            error = "Answer feedback requires an explicit owner failure judgment from this current private session.";
            return false;
        }
        learning::LearningCandidate candidate;
        const auto captured = CapturedFeedback(feedback, profile, settings.llm, fastLlmSettings, expertLlmSettings, settings.intelligence);
        if (!learning::BuildQualityLearningCandidate(captured, candidate, error) || !InitializeLearningStudio(error))
        {
            return false;
        }
        const auto admitted = [this, origin = feedback.origin, stopToken] { return !stopToken.stop_requested() && Admits(origin); };
        if (!selfAssessment.RecordJudgedQualityFailure(captured, taskId, error, admitted) || OwnerMutationCanceled(stopToken, error) ||
            !learningRecords->Propose(candidate, recordId, error))
        {
            return false;
        }
        const auto record = learningRecords->Find(recordId);
        if (!record)
        {
            error = "The exact private candidate receipt is unavailable.";
            return false;
        }
        if (record->decision != learning::LearningDecision::Accept && record->decision != learning::LearningDecision::Reject &&
            (OwnerMutationCanceled(stopToken, error) ||
                !learningRecords->Check(recordId,
                    {record->digest, true, !memory::ContainsSensitiveContent(candidate.lesson.statement + candidate.lesson.evidence), true},
                    error)))
        {
            return false;
        }
    }
    PublishComponent(
        "Quality feedback", "Pending review", "The exact owner judgment was recorded; its factual lesson awaits ordinary learning review.");
    return true;
}

bool ReviaSession::RetestQualityFeedback(const std::string& taskId, const learning::QualityFeedback& feedback, std::string& error,
    const bool ownerRequested, const std::stop_token stopToken)
{
    if (OwnerMutationCanceled(stopToken, error))
    {
        return false;
    }
    {
        const std::scoped_lock lock(operationMutex, learningStudioMutex, audienceMutex);
        if (OwnerMutationCanceled(stopToken, error))
        {
            return false;
        }
        const RuntimeStamp current = Stamp();
        if (feedback.source != learning::QualityEvidenceSource::OwnerJudgment || !Admits(feedback.origin) ||
            !feedback.origin.SameSession(current) || feedback.origin.policyVersion != current.policyVersion ||
            feedback.audienceRevision == 0 || feedback.audienceRevision != configuredAudience.revision ||
            configuredAudience.kind != identity::AudienceKind::Private || lastInputAudience != identity::AudienceKind::Private)
        {
            error = "Quality retest requires an explicit owner criterion judgment from this current private session.";
            return false;
        }
        const auto captured = CapturedFeedback(feedback, profile, settings.llm, fastLlmSettings, expertLlmSettings, settings.intelligence);
        if (!selfAssessment.RecordJudgedQualityRetest(taskId, captured, error, ownerRequested,
                [this, origin = feedback.origin, stopToken] { return !stopToken.stop_requested() && Admits(origin); }))
        {
            return false;
        }
    }
    PublishComponent("Quality feedback", feedback.criterionSatisfied ? "Criterion satisfied" : "Criterion unsatisfied",
        feedback.criterionSatisfied ? "Fresh exact-result owner judgment satisfied the recorded criterion."
                                    : "The bounded retest retained the observed quality gap.");
    return true;
}

bool ReviaSession::ReviseMemoryOwnerRequested(const memory::MemoryRevisionRequest& request, memory::MemoryRevisionReceipt& receipt,
    std::string& error, const std::stop_token stopToken)
{
    receipt = {};
    if (OwnerMutationCanceled(stopToken, error))
    {
        return false;
    }
    {
        const std::scoped_lock lock(operationMutex, learningStudioMutex, audienceMutex);
        if (OwnerMutationCanceled(stopToken, error))
        {
            return false;
        }
        const RuntimeStamp current = Stamp();
        if (!Admits(request.origin) || !request.origin.SameSession(current) || request.origin.policyVersion != current.policyVersion ||
            request.audienceRevision == 0 || request.audienceRevision != configuredAudience.revision ||
            configuredAudience.kind != identity::AudienceKind::Private || lastInputAudience != identity::AudienceKind::Private)
        {
            error = "Memory revision requires the exact owner's request from this current private session.";
            return false;
        }
        if (turnCoordinator.Memory().SubmitOwnerMemoryRevision(router, request, receipt, error, stopToken) ==
            agents::LearnedFindingResult::Failed)
        {
            return false;
        }
    }
    PublishComponent(
        "Memory", "Revised", "The explicitly selected memory and its replacement retain an exact durable owner revision receipt.");
    return true;
}
} // namespace revia::runtime
