#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
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

    // Reading the history back is what makes it a history rather than an append-only
    // sink. Without this the category guards reset with the process, so every restart
    // could raise a fresh task for a problem already recorded -- which is the one thing
    // the file existed to prevent.
    snapshot.openTasks.clear();
    slowTurnTaskCreated = false;
    voiceTaskCreated = false;
    reliabilityTaskCreated = false;
    malformedHistoryRecords = 0;

    std::ifstream file(path);
    if (!file.is_open())
    {
        // No history yet is the normal first run, not a failure.
        outError.clear();
        return true;
    }

    std::string line;
    std::unordered_map<std::string, SelfImprovementTask> byId;
    std::vector<std::string> order;
    std::unordered_set<std::string> resolved;
    while (std::getline(file, line))
    {
        if (line.empty()) continue;
        // Parsed per line, so a partially written final record -- the shape a crash
        // leaves behind -- costs that record and nothing before it.
        const json record = json::parse(line, nullptr, false);
        if (record.is_discarded() || !record.is_object())
        {
            ++malformedHistoryRecords;
            continue;
        }
        const std::string id = record.value("id", std::string{});
        if (id.empty())
        {
            ++malformedHistoryRecords;
            continue;
        }
        // A resolution retires a task rather than resurrecting it.
        if (record.value("resolved", false))
        {
            resolved.insert(id);
            continue;
        }
        SelfImprovementTask task;
        task.id = id;
        task.category = record.value("category", std::string{});
        task.observedProblem = record.value("observedProblem", std::string{});
        task.evidence = record.value("evidence", std::string{});
        task.confidence = record.value("confidence", 0.0);
        task.expectedBenefit = record.value("expectedBenefit", 0.0);
        task.estimatedRisk = record.value("estimatedRisk", 0.0);
        task.relatedMetrics = record.value("relatedMetrics", std::vector<std::string>{});
        task.relatedComponents =
            record.value("relatedComponents", std::vector<std::string>{});
        task.researchAllowed = record.value("researchAllowed", false);
        task.researchCompleted = record.value("researchCompleted", false);
        if (byId.find(id) == byId.end()) order.push_back(id);
        byId[id] = std::move(task);
    }

    for (const std::string& id : order)
    {
        if (resolved.count(id) != 0) continue;
        const SelfImprovementTask& task = byId[id];
        snapshot.openTasks.push_back(task);
        // The guards follow what is still open, not what this process created. That is
        // the difference between "already raised" and "raised since the last restart".
        if (task.category == "performance") slowTurnTaskCreated = true;
        else if (task.category == "voice") voiceTaskCreated = true;
        else if (task.category == "reliability") reliabilityTaskCreated = true;
    }

    outError.clear();
    return true;
}

std::size_t SelfAssessmentEngine::MalformedHistoryRecords() const
{
    std::lock_guard lock(mutex);
    return malformedHistoryRecords;
}

bool SelfAssessmentEngine::ResolveTask(const std::string& taskId, std::string& outError)
{
    SelfImprovementTask retired;
    {
        std::lock_guard lock(mutex);
        const auto found = std::find_if(
            snapshot.openTasks.begin(), snapshot.openTasks.end(),
            [&taskId](const SelfImprovementTask& task) { return task.id == taskId; });
        if (found == snapshot.openTasks.end())
        {
            outError = "No open self-improvement task with that id.";
            return false;
        }
        retired = *found;
        snapshot.openTasks.erase(found);
    }
    // Appended rather than rewritten: the history stays append-only and a resolution is
    // simply another record the loader honours.
    return PersistResolution(retired, outError);
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
    // A task that could not be written is not durable, and saying so beats a panel
    // that lists it as recorded.
    for (const SelfImprovementTask& task : created)
    {
        if (!PersistTask(task))
        {
            snapshot.conclusion += " (A self-improvement task could not be saved: " +
                LastPersistenceError() + ")";
        }
    }
    return Snapshot();
}

SelfAssessmentSnapshot SelfAssessmentEngine::Snapshot() const
{
    std::lock_guard lock(mutex);
    return snapshot;
}

bool SelfAssessmentEngine::AppendRecord(const json& record, std::string& outError)
{
    std::filesystem::path destination;
    {
        std::lock_guard lock(mutex);
        destination = path;
    }
    if (destination.empty())
    {
        outError = "The self-improvement history has no configured path.";
        return false;
    }
    std::ofstream file(destination, std::ios::app);
    if (!file.is_open())
    {
        outError = "The self-improvement history could not be opened for writing: " +
            destination.string();
        return false;
    }
    file << record.dump() << '\n';
    // Flushed, then checked. The previous version tested only that the stream opened,
    // so a full disk or a revoked permission produced a task the panel listed as
    // durable and the next start had never heard of.
    file.flush();
    if (!file.good())
    {
        outError = "The self-improvement history could not be written: " +
            destination.string();
        return false;
    }
    return true;
}

bool SelfAssessmentEngine::PersistTask(const SelfImprovementTask& task)
{
    const json record = {
        {"id", task.id}, {"category", task.category},
        {"observedProblem", task.observedProblem}, {"evidence", task.evidence},
        {"confidence", task.confidence}, {"expectedBenefit", task.expectedBenefit},
        {"estimatedRisk", task.estimatedRisk}, {"relatedMetrics", task.relatedMetrics},
        {"relatedComponents", task.relatedComponents},
        {"researchAllowed", task.researchAllowed},
        {"researchCompleted", task.researchCompleted},
        {"resolved", false}
    };
    std::string error;
    if (AppendRecord(record, error)) return true;
    std::lock_guard lock(mutex);
    lastPersistenceError = error;
    return false;
}

bool SelfAssessmentEngine::PersistResolution(
    const SelfImprovementTask& task,
    std::string& outError)
{
    const json record = {
        {"id", task.id}, {"category", task.category}, {"resolved", true}};
    if (AppendRecord(record, outError)) return true;
    std::lock_guard lock(mutex);
    lastPersistenceError = outError;
    return false;
}

std::string SelfAssessmentEngine::LastPersistenceError() const
{
    std::lock_guard lock(mutex);
    return lastPersistenceError;
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
