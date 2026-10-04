#pragma once

#include "Learning/learningRecordStore.h"

#include <cstdint>
#include <map>
#include <string>

namespace revia::learning
{
enum class QualityEvidenceSource
{
    OwnerJudgment,
    DeterministicAcceptance
};

enum class QualityTarget
{
    ConversationTurn,
    AgentAttempt
};

enum class QualityIssue
{
    AnswerCoverage,
    UnsupportedClaim,
    MemoryContinuity,
    MissingAcceptanceEvidence
};

// Callers establish judgment authority against retained source/target receipts.
// Monitor flags and provider self-ratings are not judged outcomes.
struct QualityFeedback
{
    runtime::RuntimeStamp origin;
    std::uint64_t audienceRevision = 0;
    QualityEvidenceSource source = QualityEvidenceSource::OwnerJudgment;
    QualityTarget target = QualityTarget::ConversationTurn;
    QualityIssue issue = QualityIssue::AnswerCoverage;
    std::string judgmentId;
    std::string sourceId;
    std::string targetId;
    std::string targetDigest;
    std::string criterion;
    std::string evidence;
    std::map<std::string, std::string> dependencies;
    std::string correctsRecordId;
    bool criterionSatisfied = false;
    bool privateContextExcluded = false;
};

[[nodiscard]] std::string ToString(QualityTarget value);
[[nodiscard]] std::string ToString(QualityIssue value);
[[nodiscard]] bool ValidateQualityFeedback(const QualityFeedback& feedback, std::string& outError);
[[nodiscard]] std::string QualityFeedbackDigest(const QualityFeedback& feedback);
[[nodiscard]] std::string QualityCriterionKey(const QualityFeedback& feedback);
[[nodiscard]] std::string QualityFeedbackEvidence(const QualityFeedback& feedback);
bool BuildQualityLearningCandidate(const QualityFeedback& feedback, LearningCandidate& outCandidate, std::string& outError);
} // namespace revia::learning
