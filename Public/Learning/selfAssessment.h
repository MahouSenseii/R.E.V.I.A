#pragma once

#include "Runtime/runtimeEvents.h"

#include <chrono>
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

private:
    void PersistTask(const SelfImprovementTask& task);
    mutable std::mutex mutex;
    std::filesystem::path path;
    SelfAssessmentSnapshot snapshot;
    bool slowTurnTaskCreated = false;
    bool voiceTaskCreated = false;
    bool reliabilityTaskCreated = false;
};

} // namespace revia::learning
