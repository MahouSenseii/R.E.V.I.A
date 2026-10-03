#pragma once

#include "Learning/learningReview.h"
#include "Runtime/runtimeStamp.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::learning
{

enum class LearningDecision
{
    Pending,
    Accept,
    Rework,
    Reject,
    NeedEvidence
};

enum class LearningDisposition
{
    PrivateCandidate,
    AcceptedAwaitingMemory,
    MemorySaveFailed,
    TrustedMemory
};

struct LearningEvidence
{
    std::vector<std::string> sources;
    std::vector<std::string> conditions;
    std::vector<std::string> supporting;
    std::vector<std::string> contradicting;
    std::string modelVersion;
    std::string toolVersion;
    std::string checkedAt;
};

struct LearningCandidate
{
    Lesson lesson;
    LearningEvidence evidence;
    runtime::RuntimeStamp origin;
    std::string correctsRecordId;
};

struct LearningChecks
{
    std::string candidateDigest;
    bool verified = false;
    bool privacySafe = false;
    bool deduplicated = false;
};

struct LearningReviewDecision
{
    LearningDecision decision = LearningDecision::Pending;
    std::string feedback;
};

struct LearningRecord
{
    std::string id;
    std::string digest;
    LearningCandidate candidate;
    LearningChecks checks;
    std::vector<LearningReviewDecision> reviews;
    LearningDecision decision = LearningDecision::Pending;
    LearningDisposition disposition = LearningDisposition::PrivateCandidate;
    std::string memoryId;
};

// Private review/history only. Runtime submits eligible content to the existing MemoryAgent.
class LearningRecordStore
{
public:
    using AdmissionGuard = std::function<bool()>;
    explicit LearningRecordStore(std::filesystem::path directory, AdmissionGuard admission = {});
    bool Initialize(std::string& outError);
    bool Propose(const LearningCandidate& candidate, std::string& outId, std::string& outError);
    bool Check(const std::string& id, const LearningChecks& checks, std::string& outError);
    bool Decide(const std::string& id, LearningDecision decision, const std::string& feedback, std::string& outError);
    [[nodiscard]] std::optional<LearningRecord> Find(const std::string& id) const;
    [[nodiscard]] std::vector<LearningRecord> History() const;
    [[nodiscard]] std::optional<LearningRecord> PendingAdmission(const std::string& id) const;
    bool RecordAdmission(const std::string& id, bool saved, const std::string& memoryId, std::string& outError);

private:
    bool Save(const std::vector<LearningRecord>& records, std::string& outError) const;
    [[nodiscard]] bool Admitted() const;
    mutable std::mutex mutex;
    std::filesystem::path directory;
    AdmissionGuard admission;
    std::vector<LearningRecord> records;
    bool initialized = false;
};

} // namespace revia::learning
