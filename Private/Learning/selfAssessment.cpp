#include "Learning/selfAssessment.h"

#include "Actions/actionTypes.h"

#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

namespace revia::learning
{
namespace
{
using nlohmann::json;

std::string Percentage(const std::uint64_t part, const std::uint64_t whole)
{
    std::ostringstream output;
    output << std::fixed << std::setprecision(1)
        << (whole == 0 ? 0.0 : static_cast<double>(part) * 100.0 / whole) << '%';
    return output.str();
}
}

bool SelfAssessmentEngine::Initialize(
    const std::filesystem::path& historyPath,
    std::string& outError)
{
    std::lock_guard lock(mutex);
    path = historyPath;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
    {
        outError = "Could not create self-improvement history: " + error.message();
        path.clear();
        return false;
    }
    outError.clear();
    return true;
}

void SelfAssessmentEngine::Observe(const runtime::RuntimeEvent& event)
{
    std::lock_guard lock(mutex);
    if (event.component == "Conversation" && event.phase == "Ready")
    {
        ++snapshot.conversationTurns;
        if (event.elapsedMilliseconds > 8000.0) ++snapshot.slowTurns;
    }
    if (event.component == "Intelligence router" &&
        (event.phase == "Expert" || event.phase == "ExpertVision"))
        ++snapshot.expertRoutes;
    if (event.component == "Internet" && event.phase == "Unavailable")
        ++snapshot.internetFailures;
    if (event.component == "Memory" &&
        (event.phase == "Error" || event.phase == "Unavailable"))
        ++snapshot.memoryFailures;
    if (event.component == "Voice" &&
        ((event.phase == "Generated" && event.elapsedMilliseconds > 12000.0) ||
         (event.phase == "FirstAudioPlayed" && event.elapsedMilliseconds > 15000.0) ||
         event.phase == "Error"))
        ++snapshot.voiceStalls;
}

SelfAssessmentSnapshot SelfAssessmentEngine::Assess()
{
    std::vector<SelfImprovementTask> created;
    {
        std::lock_guard lock(mutex);
        if (!slowTurnTaskCreated && snapshot.conversationTurns >= 8 &&
            snapshot.slowTurns * 4 >= snapshot.conversationTurns)
        {
            SelfImprovementTask task;
            task.id = actions::NewActionId();
            task.category = "conversation_latency";
            task.observedProblem = "Interactive conversation is frequently slower than 8 seconds.";
            task.evidence = std::to_string(snapshot.slowTurns) + " of " +
                std::to_string(snapshot.conversationTurns) + " completed turns exceeded 8 seconds (" +
                Percentage(snapshot.slowTurns, snapshot.conversationTurns) + ").";
            task.confidence = 0.82;
            task.expectedBenefit = 0.75;
            task.estimatedRisk = 0.2;
            task.relatedMetrics = {"turn_total", "first_token", "selected_tier"};
            task.relatedComponents = {"Intelligence router", "llama.cpp", "Context builder"};
            task.researchAllowed = true;
            snapshot.openTasks.push_back(task);
            created.push_back(task);
            slowTurnTaskCreated = true;
        }
        if (!voiceTaskCreated && snapshot.voiceStalls >= 5)
        {
            SelfImprovementTask task;
            task.id = actions::NewActionId();
            task.category = "first_audio_latency";
            task.observedProblem = "Voice phrase generation repeatedly exceeds 12 seconds.";
            task.evidence = std::to_string(snapshot.voiceStalls) +
                " generated phrases or voice errors crossed the stall threshold.";
            task.confidence = 0.86;
            task.expectedBenefit = 0.8;
            task.estimatedRisk = 0.25;
            task.relatedMetrics = {"qwen_synthesis", "queue_depth", "device"};
            task.relatedComponents = {"Qwen3-TTS", "ReplyFragmenter", "Resource planner"};
            task.researchAllowed = true;
            snapshot.openTasks.push_back(task);
            created.push_back(task);
            voiceTaskCreated = true;
        }
        const std::uint64_t reliabilityFailures =
            snapshot.internetFailures + snapshot.memoryFailures;
        if (!reliabilityTaskCreated && reliabilityFailures >= 5)
        {
            SelfImprovementTask task;
            task.id = actions::NewActionId();
            task.category = "runtime_reliability";
            task.observedProblem = "A local support service is failing repeatedly.";
            task.evidence = std::to_string(snapshot.internetFailures) +
                " internet failures and " + std::to_string(snapshot.memoryFailures) +
                " memory failures were observed.";
            task.confidence = 0.8;
            task.expectedBenefit = 0.65;
            task.estimatedRisk = 0.15;
            task.relatedMetrics = {"failure_count", "component_phase"};
            task.relatedComponents = {"Internet", "Memory"};
            task.researchAllowed = true;
            snapshot.openTasks.push_back(task);
            created.push_back(task);
            reliabilityTaskCreated = true;
        }
        snapshot.conclusion = snapshot.openTasks.empty()
            ? snapshot.conversationTurns < 8
                ? "Not enough evidence yet. No change recommended."
                : "No measured threshold is currently crossed. No change recommended."
            : std::to_string(snapshot.openTasks.size()) +
                " evidence-backed improvement task(s) are ready for review.";
    }
    for (const SelfImprovementTask& task : created) PersistTask(task);
    return Snapshot();
}

SelfAssessmentSnapshot SelfAssessmentEngine::Snapshot() const
{
    std::lock_guard lock(mutex);
    return snapshot;
}

void SelfAssessmentEngine::PersistTask(const SelfImprovementTask& task)
{
    std::filesystem::path destination;
    {
        std::lock_guard lock(mutex);
        destination = path;
    }
    if (destination.empty()) return;
    const json record = {
        {"id", task.id}, {"category", task.category},
        {"observedProblem", task.observedProblem}, {"evidence", task.evidence},
        {"confidence", task.confidence}, {"expectedBenefit", task.expectedBenefit},
        {"estimatedRisk", task.estimatedRisk}, {"relatedMetrics", task.relatedMetrics},
        {"relatedComponents", task.relatedComponents},
        {"researchAllowed", task.researchAllowed},
        {"researchCompleted", task.researchCompleted}
    };
    std::ofstream file(destination, std::ios::app);
    if (file.is_open()) file << record.dump() << '\n';
}

std::string SelfAssessmentEngine::Report() const
{
    const SelfAssessmentSnapshot current = Snapshot();
    std::ostringstream output;
    output << "Self-assessment uses observed runtime evidence only. It cannot apply its "
              "own changes.\n\n"
        << "Conversation turns: " << current.conversationTurns
        << " (slow: " << current.slowTurns << ")\n"
        << "Expert routes: " << current.expertRoutes << "\n"
        << "Voice stalls/errors: " << current.voiceStalls << "\n"
        << "Internet failures: " << current.internetFailures << "\n"
        << "Memory failures: " << current.memoryFailures << "\n\n"
        << current.conclusion;
    for (const SelfImprovementTask& task : current.openTasks)
    {
        output << "\n\n" << task.id << " [" << task.category << "]\n"
            << task.observedProblem << "\nEvidence: " << task.evidence
            << "\nConfidence: " << static_cast<int>(task.confidence * 100.0) << '%';
    }
    return output.str();
}

} // namespace revia::learning
