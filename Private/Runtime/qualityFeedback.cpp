#include "Runtime/reviaSession.h"

#include "Audit/contentDigest.h"
#include "Memory/sensitiveContent.h"

#include <algorithm>
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

bool SameCorrectionScope(const memory::MemoryScope& left, const memory::MemoryScope& right)
{
    return memory::IsAttributedPrivateScope(left) && memory::IsAttributedPrivateScope(right) &&
        left.participantId == right.participantId && left.participantSource == right.participantSource &&
        left.consentRevision == right.consentRevision && left.companionId == right.companionId &&
        left.audience.kind == right.audience.kind && left.audience.audienceId == right.audience.audienceId &&
        left.audience.revision == right.audience.revision && left.audience.recipientEntityIds == right.audience.recipientEntityIds;
}

struct OwnerRevisionScopeLease final : memory::MemoryRevisionScopeLease
{
    OwnerRevisionScopeLease(std::mutex& speaker, std::mutex& audience) : locks(speaker, audience) {}
    std::scoped_lock<std::mutex, std::mutex> locks;
};

bool BoundedCorrection(const memory::MemoryCorrectionProposal& proposal)
{
    const auto id = [](const std::string& value) { return !value.empty() && value.size() <= 128; };
    return id(proposal.id) && id(proposal.original.id) && id(proposal.proposedMemoryId) &&
        proposal.original.id != proposal.proposedMemoryId && proposal.turnId != 0 &&
        !proposal.original.summary.empty() && proposal.original.summary.size() <= 4096 &&
        proposal.expectedSummaryDigest == audit::ContentDigest(proposal.original.summary) &&
        proposal.original.revisionReceiptId.size() <= 128 && proposal.original.revisionChainId.size() <= 128 &&
        (proposal.original.currentRevisionId.empty() || proposal.original.currentRevisionId == proposal.original.id) &&
        proposal.original.subject == memory::ParticipantSubject(proposal.scope) &&
        proposal.corrected.subject == proposal.original.subject && proposal.corrected.bSuccess &&
        proposal.corrected.bShouldRemember && !proposal.corrected.bPreempted &&
        !proposal.corrected.summary.empty() && proposal.corrected.summary.size() <= 4096 &&
        proposal.corrected.summary != proposal.original.summary && !proposal.corrected.category.empty() &&
        proposal.corrected.category.size() <= 128 && proposal.corrected.category == proposal.original.category &&
        !proposal.evidence.empty() && proposal.evidence.size() <= 2048;
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
        const std::scoped_lock lock(operationMutex, learningStudioMutex);
        if (!ReviseMemoryOwnerRequestedLocked(request, receipt, error, stopToken))
            return false;
    }
    PublishComponent(
        "Memory", "Revised", "The explicitly selected memory and its replacement retain an exact durable owner revision receipt.");
    return true;
}

bool ReviaSession::ReviseMemoryOwnerRequestedLocked(const memory::MemoryRevisionRequest& request,
    memory::MemoryRevisionReceipt& receipt, std::string& error, const std::stop_token stopToken,
    std::optional<memory::MemoryScope> proposalScope)
{
    if (OwnerMutationCanceled(stopToken, error)) return false;
    auto acquireScope = [this, origin = request.origin, audienceRevision = request.audienceRevision,
                            proposalScope = std::move(proposalScope), stopToken, &error]() -> std::unique_ptr<memory::MemoryRevisionScopeLease>
    {
        // The store acquires this lease after its database mutex and releases it
        // before optional embedding work. Background saves use that same order.
        auto lease = std::make_unique<OwnerRevisionScopeLease>(speakerMutex, audienceMutex);
        if (OwnerMutationCanceled(stopToken, error)) return {};
        const RuntimeStamp current = Stamp();
        if (!Admits(origin) || !origin.SameSession(current) || origin.policyVersion != current.policyVersion ||
            audienceRevision == 0 || audienceRevision != configuredAudience.revision ||
            configuredAudience.kind != identity::AudienceKind::Private || lastInputAudience != identity::AudienceKind::Private ||
            (proposalScope && !SameCorrectionScope(*proposalScope, CapturePrivateMemoryScopeLocked())))
        {
            error = "Memory revision requires the exact owner's request from this current private session.";
            return {};
        }
        return lease;
    };
    return turnCoordinator.Memory().SubmitOwnerMemoryRevision(router, request, receipt, error, stopToken, std::move(acquireScope)) !=
        agents::LearnedFindingResult::Failed;
}

void ReviaSession::PruneMemoryCorrectionsLocked(const memory::MemoryScope& scope, const RuntimeStamp& origin)
{
    std::erase_if(memoryCorrections, [&](const auto& pending)
    {
        return !Admits(pending.origin) || !pending.origin.SameSession(origin) ||
            pending.origin.policyVersion != origin.policyVersion || !SameCorrectionScope(pending.proposal.scope, scope);
    });
}

void ReviaSession::QueueMemoryCorrectionProposals(
    const std::vector<memory::MemoryCorrectionProposal>& proposals, const RuntimeStamp& origin)
{
    bool queued = false;
    {
        const std::scoped_lock lock(speakerMutex, audienceMutex, memoryCorrectionMutex);
        const auto current = Stamp();
        const auto scope = CapturePrivateMemoryScopeLocked();
        PruneMemoryCorrectionsLocked(scope, current);
        if (!Admits(origin) || !origin.SameSession(current) || origin.policyVersion != current.policyVersion) return;
        for (const auto& proposal : proposals)
        {
            if (!SameCorrectionScope(proposal.scope, scope) || !BoundedCorrection(proposal)) continue;
            const auto duplicate = std::find_if(memoryCorrections.begin(), memoryCorrections.end(), [&](const auto& pending)
            {
                return pending.proposal.id == proposal.id || (pending.proposal.original.id == proposal.original.id &&
                    pending.proposal.proposedMemoryId == proposal.proposedMemoryId);
            });
            if (duplicate != memoryCorrections.end()) continue;
            // Copy only review fields. Model vectors, timings and free-form sources
            // are not needed by this bounded transient queue or its UI.
            auto bounded = proposal;
            bounded.original.source.clear();
            bounded.original.createdAt.clear();
            bounded.original.revisesMemoryId.clear();
            bounded.corrected = {};
            bounded.corrected.bSuccess = bounded.corrected.bShouldRemember = true;
            bounded.corrected.category = proposal.corrected.category;
            bounded.corrected.summary = proposal.corrected.summary;
            bounded.corrected.subject = proposal.corrected.subject;
            if (memoryCorrections.size() == 32) memoryCorrections.erase(memoryCorrections.begin());
            memoryCorrections.push_back({std::move(bounded), origin});
            queued = true;
        }
    }
    if (queued)
        PublishComponent("Memory", "Review available", "A possible correction is ready for exact owner review in Memory.");
}

std::vector<memory::MemoryCorrectionProposal> ReviaSession::PendingMemoryCorrections()
{
    const std::scoped_lock lock(speakerMutex, audienceMutex, memoryCorrectionMutex);
    PruneMemoryCorrectionsLocked(CapturePrivateMemoryScopeLocked(), Stamp());
    std::vector<memory::MemoryCorrectionProposal> result;
    result.reserve(memoryCorrections.size());
    for (const auto& pending : memoryCorrections) result.push_back(pending.proposal);
    return result;
}

bool ReviaSession::AcceptMemoryCorrectionProposal(const std::string& proposalId, memory::MemoryRevisionReceipt& receipt,
    std::string& error, const std::stop_token stopToken)
{
    receipt = {};
    if (OwnerMutationCanceled(stopToken, error)) return false;
    bool accepted = false;
    {
        const std::scoped_lock lock(operationMutex, learningStudioMutex);
        if (OwnerMutationCanceled(stopToken, error)) return false;
        PendingMemoryCorrection pending;
        {
            const std::lock_guard queueLock(memoryCorrectionMutex);
            const auto found = std::find_if(memoryCorrections.begin(), memoryCorrections.end(),
                [&](const auto& value) { return value.proposal.id == proposalId; });
            if (found == memoryCorrections.end())
            {
                error = "This correction proposal is no longer available in the current private session.";
                return false;
            }
            pending = *found;
            // Consumption is only transient review state. The exact scope lease
            // and transaction still decide whether any fact may be revised.
            memoryCorrections.erase(found);
        }
        const auto& proposal = pending.proposal;
        memory::MemoryRevisionRequest request;
        request.ownerRequestId = actions::NewActionId();
        request.originalId = proposal.original.id;
        request.expectedSummaryDigest = proposal.expectedSummaryDigest;
        request.expectedSubject = proposal.original.subject;
        request.priorReceiptId = proposal.original.revisionReceiptId;
        request.expectedReplacementId = proposal.proposedMemoryId;
        request.corrected = proposal.corrected;
        request.reason = "The owner accepted this exact old-to-new correction proposal.";
        request.evidence = proposal.evidence;
        request.origin = pending.origin;
        request.audienceRevision = proposal.scope.audience.revision;
        accepted = ReviseMemoryOwnerRequestedLocked(request, receipt, error, stopToken, proposal.scope);
        // A stale/refused proposal cannot be retried with a fresh view inferred from
        // its old text. The next suggestion or manual review captures a new pair.
    }
    if (accepted)
        PublishComponent("Memory", "Revised", "The accepted correction retains an exact durable owner revision receipt.");
    return accepted;
}

bool ReviaSession::RejectMemoryCorrectionProposal(const std::string& proposalId, std::string& error, const std::stop_token stopToken)
{
    if (OwnerMutationCanceled(stopToken, error)) return false;
    const std::scoped_lock lock(speakerMutex, audienceMutex, memoryCorrectionMutex);
    if (OwnerMutationCanceled(stopToken, error)) return false;
    PruneMemoryCorrectionsLocked(CapturePrivateMemoryScopeLocked(), Stamp());
    const auto removed = std::erase_if(memoryCorrections, [&](const auto& pending) { return pending.proposal.id == proposalId; });
    if (removed == 0)
    {
        error = "This correction proposal is no longer available in the current private session.";
        return false;
    }
    return true;
}
} // namespace revia::runtime
