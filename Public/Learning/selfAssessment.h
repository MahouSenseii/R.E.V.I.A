#pragma once

#include "Runtime/runtimeEvents.h"

#include <nlohmann/json_fwd.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace revia::learning
{

struct SelfImprovementTask
{
    std::string id;
    std::string category;
    std::string observedProblem;
    std::string evidence;
    double confidence = 0.0;
    double expectedBenefit = 0.0;
    double estimatedRisk = 0.0;
    std::vector<std::string> relatedMetrics;
    std::vector<std::string> relatedComponents;
    bool researchAllowed = false;
    bool researchCompleted = false;
};

struct ImprovementProposal
{
    std::string title;
    std::string problem;
    std::string evidence;
    std::string proposedChange;
    std::string expectedBenefit;
    std::string risks;
    std::vector<std::string> affectedComponents;
    std::string benchmarkPlan;
    std::string rollbackPlan;
    std::vector<std::string> sources;
    double confidence = 0.0;
};

struct SelfAssessmentSnapshot
{
    std::uint64_t conversationTurns = 0;
    std::uint64_t slowTurns = 0;
    std::uint64_t expertRoutes = 0;
    std::uint64_t internetFailures = 0;
    std::uint64_t memoryFailures = 0;
    std::uint64_t voiceStalls = 0;
    std::vector<SelfImprovementTask> openTasks;
    std::string conclusion = "Not enough evidence yet.";
};

// Evidence collector only. It may prepare a proposal, but cannot change settings,
// install a model, edit source, or widen a capability.
class SelfAssessmentEngine
{
public:
    bool Initialize(const std::filesystem::path& historyPath, std::string& outError);
    void Observe(const runtime::RuntimeEvent& event);
    SelfAssessmentSnapshot Assess();
    [[nodiscard]] SelfAssessmentSnapshot Snapshot() const;
    [[nodiscard]] std::string Report() const;

    // Retires an open task. Appended to the history as a resolution record, so the
    // next start does not raise it again -- and does not resurrect it either.
    bool ResolveTask(const std::string& taskId, std::string& outError);
    // History lines that could not be parsed on load. A partially written final record
    // is expected after a crash; a rising count is not.
    [[nodiscard]] std::size_t MalformedHistoryRecords() const;
    // Empty unless a write failed. A task the panel shows as recorded must actually be
    // on disk, and this is how a caller finds out it is not.
    [[nodiscard]] std::string LastPersistenceError() const;

private:
    [[nodiscard]] bool AppendRecord(const nlohmann::json& record, std::string& outError);
    [[nodiscard]] bool PersistTask(const SelfImprovementTask& task);
    [[nodiscard]] bool PersistResolution(
        const SelfImprovementTask& task,
        std::string& outError);
    mutable std::mutex mutex;
    std::filesystem::path path;
    SelfAssessmentSnapshot snapshot;
    // Guards against raising a second task for a problem already recorded. Restored
    // from the history on Initialize rather than reset with the process.
    bool slowTurnTaskCreated = false;
    bool voiceTaskCreated = false;
    bool reliabilityTaskCreated = false;
    std::size_t malformedHistoryRecords = 0;
    std::string lastPersistenceError;
};

} // namespace revia::learning
