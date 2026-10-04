#include "Learning/qualityFeedback.h"

#include "Audit/contentDigest.h"
#include "Memory/sensitiveContent.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <sstream>

namespace revia::learning
{
namespace
{
using nlohmann::json;

json FeedbackJson(const QualityFeedback& value)
{
    return {{"source", static_cast<int>(value.source)}, {"target", static_cast<int>(value.target)},
        {"issue", static_cast<int>(value.issue)}, {"judgmentId", value.judgmentId}, {"sourceId", value.sourceId},
        {"targetId", value.targetId}, {"targetDigest", value.targetDigest}, {"criterion", value.criterion}, {"evidence", value.evidence},
        {"dependencies", value.dependencies}, {"correctsRecordId", value.correctsRecordId},
        {"criterionSatisfied", value.criterionSatisfied}, {"privateContextExcluded", value.privateContextExcluded},
        {"audienceRevision", value.audienceRevision},
        {"origin",
            {{"companionId", value.origin.companionId}, {"sessionId", value.origin.sessionId}, {"generation", value.origin.generation},
                {"taskId", value.origin.taskId}, {"attemptId", value.origin.attemptId}, {"policyVersion", value.origin.policyVersion}}}};
}

bool Bounded(const std::string& value, const std::size_t maximum)
{
    return !value.empty() && value.size() <= maximum && value.find('\0') == std::string::npos;
}
}

std::string ToString(const QualityTarget value)
{
    switch (value)
    {
    case QualityTarget::ConversationTurn:
        return "conversation turn";
    case QualityTarget::AgentAttempt:
        return "agent attempt";
    }
    return "unsupported";
}

std::string ToString(const QualityIssue value)
{
    switch (value)
    {
    case QualityIssue::AnswerCoverage:
        return "answer coverage";
    case QualityIssue::UnsupportedClaim:
        return "unsupported claim";
    case QualityIssue::MemoryContinuity:
        return "memory continuity";
    case QualityIssue::MissingAcceptanceEvidence:
        return "missing acceptance evidence";
    }
    return "unsupported";
}

bool ValidateQualityFeedback(const QualityFeedback& value, std::string& error)
{
    if ((value.source != QualityEvidenceSource::OwnerJudgment && value.source != QualityEvidenceSource::DeterministicAcceptance) ||
        (value.target != QualityTarget::ConversationTurn && value.target != QualityTarget::AgentAttempt) ||
        value.issue < QualityIssue::AnswerCoverage || value.issue > QualityIssue::MissingAcceptanceEvidence ||
        !Bounded(value.judgmentId, 128) || !Bounded(value.sourceId, 128) || !Bounded(value.targetId, 128) ||
        value.targetDigest.size() != 64 ||
        !std::all_of(value.targetDigest.begin(), value.targetDigest.end(),
            [](const char character) { return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'); }) ||
        !Bounded(value.criterion, 1000) || !Bounded(value.evidence, 2048) || !Bounded(value.origin.companionId, 128) ||
        !Bounded(value.origin.sessionId, 128) || value.origin.generation == 0 || value.audienceRevision == 0 ||
        value.origin.taskId.size() > 128 || value.origin.attemptId.size() > 128 || value.correctsRecordId.size() > 80 ||
        value.dependencies.empty() || value.dependencies.size() > 16 || value.privateContextExcluded)
    {
        error = "Quality feedback needs an admitted exact target, finite judgment source and bounded criterion evidence.";
        return false;
    }
    for (const auto& [name, version] : value.dependencies)
    {
        if (!Bounded(name, 128) || !Bounded(version, 256))
        {
            error = "Quality feedback requires bounded relevant dependency fingerprints.";
            return false;
        }
    }
    if (memory::ContainsSensitiveContent(FeedbackJson(value).dump()))
    {
        error = "Quality feedback contains excluded private or sensitive content.";
        return false;
    }
    error.clear();
    return true;
}

std::string QualityFeedbackDigest(const QualityFeedback& value)
{
    return audit::ContentDigest(FeedbackJson(value).dump());
}

std::string QualityCriterionKey(const QualityFeedback& value)
{
    return "quality:" + ToString(value.target) + ":" + ToString(value.issue) + ":" + audit::ContentDigest(value.criterion);
}

std::string QualityFeedbackEvidence(const QualityFeedback& value)
{
    std::ostringstream output;
    output << (value.source == QualityEvidenceSource::OwnerJudgment ? "Owner judgment " : "Native acceptance judgment ") << value.judgmentId
           << "; source " << value.sourceId << "; " << ToString(value.target) << ' ' << value.targetId << "; target SHA256 "
           << value.targetDigest << "; audience revision " << value.audienceRevision << "; criterion: " << value.criterion
           << "; result: " << (value.criterionSatisfied ? "satisfied" : "unsatisfied") << "; evidence: " << value.evidence
           << "; judgment SHA256 " << QualityFeedbackDigest(value);
    return output.str();
}

bool BuildQualityLearningCandidate(const QualityFeedback& value, LearningCandidate& candidate, std::string& error)
{
    candidate = {};
    if (!ValidateQualityFeedback(value, error))
    {
        return false;
    }
    candidate.origin = value.origin;
    candidate.correctsRecordId = value.correctsRecordId;
    candidate.lesson.id = "quality-" + QualityFeedbackDigest(value);
    candidate.lesson.kind = LessonKind::Quality;
    candidate.lesson.statement = "The cited " + ToString(value.target) + " was explicitly judged against the " + ToString(value.issue) +
                                 " criterion and was " + (value.criterionSatisfied ? "satisfactory" : "unsatisfactory") +
                                 ". This records one scoped judgment; it establishes no general cause or personality preference.";
    candidate.lesson.evidence = QualityFeedbackEvidence(value);
    candidate.lesson.sampleSize = 1;
    candidate.evidence.sources = {value.sourceId, value.targetId, "SHA256 " + value.targetDigest};
    candidate.evidence.conditions = {
        "Explicit exact-result judgment; diagnostic flags, transport success and model self-ratings are not authority.",
        "Criterion: " + value.criterion, "Configuration fingerprints identify captured settings, not exact model weight bytes."};
    candidate.evidence.supporting = {"Exact judgment SHA256 " + QualityFeedbackDigest(value), "Target SHA256 " + value.targetDigest};
    for (const auto& [name, version] : value.dependencies)
    {
        candidate.evidence.conditions.push_back("Dependency " + name + ": " + version);
    }
    candidate.evidence.toolVersion = "judged-quality-v1";
    if (const auto model = value.dependencies.find("model"); model != value.dependencies.end())
    {
        candidate.evidence.modelVersion = model->second;
    }
    candidate.evidence.checkedAt = "judgment " + audit::ContentDigest(value.judgmentId).substr(0, 32);
    return true;
}
} // namespace revia::learning
