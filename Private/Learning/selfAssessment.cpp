#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include "Learning/selfAssessment.h"

#include "Actions/actionTypes.h"
#include "Memory/sensitiveContent.h"

#include <fstream>
#include <cmath>
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

// The one place each problem kind is named. Assess() writes these and Initialize()
// stores back whatever it read, so there is no second vocabulary to fall out of step
// with this one.
constexpr const char* ConversationLatencyCategory = "conversation_latency";
constexpr const char* FirstAudioLatencyCategory = "first_audio_latency";
constexpr const char* RuntimeReliabilityCategory = "runtime_reliability";

json GapJson(const CapabilityGapObservation& value)
{
    return {{"goal", value.goal}, {"attempts", value.attempts}, {"successes", value.successes}, {"failures", value.failures},
        {"reason", static_cast<int>(value.reason)}, {"cause", value.cause}, {"causeProven", value.causeProven},
        {"missingCapability", value.missingCapability}, {"evidence", value.evidence}, {"partialEffects", value.partialEffects},
        {"nextStep", value.nextStep}, {"implementationHints", value.implementationHints}, {"retestConditions", value.retestConditions},
        {"owner", value.owner}, {"priority", value.priority}, {"dependencies", value.dependencies}};
}

bool ValidGap(const CapabilityGapObservation& value)
{
    const auto bounded = [](const std::vector<std::string>& values)
    {
        return values.size() <= 16 && std::all_of(values.begin(), values.end(),
            [](const auto& text) { return !text.empty() && text.size() <= 1024; });
    };
    if (value.goal.empty() || value.goal.size() > 1024 || value.attempts.empty() || value.failures.empty() || !bounded(value.attempts) ||
        !bounded(value.successes) || !bounded(value.failures) || static_cast<int>(value.reason) < 0 || static_cast<int>(value.reason) > 5 ||
        value.cause.size() > 1024 || value.missingCapability.size() > 1024 ||
        (value.reason == CapabilityGapReason::MissingCapability && value.missingCapability.empty()) || value.evidence.empty() ||
        value.evidence.size() > 4096 || value.partialEffects.size() > 1024 || value.nextStep.empty() || value.nextStep.size() > 1024 ||
        value.implementationHints.size() > 1024 || value.retestConditions.empty() || value.retestConditions.size() > 1024 ||
        value.owner.empty() || value.owner.size() > 128 || value.priority > 10 || value.dependencies.empty() || value.dependencies.size() > 16)
        return false;
    for (const auto& [name, version] : value.dependencies)
        if (name.empty() || name.size() > 128 || version.empty() || version.size() > 256) return false;
    return !memory::ContainsSensitiveContent(GapJson(value).dump());
}

CapabilityGapObservation DecodeGap(const json& value)
{
    CapabilityGapObservation result;
    result.goal = value.at("goal").get<std::string>();
    result.attempts = value.at("attempts").get<std::vector<std::string>>();
    result.successes = value.at("successes").get<std::vector<std::string>>();
    result.failures = value.at("failures").get<std::vector<std::string>>();
    result.reason = static_cast<CapabilityGapReason>(value.at("reason").get<int>());
    result.cause = value.at("cause").get<std::string>();
    result.causeProven = value.at("causeProven").get<bool>();
    result.missingCapability = value.at("missingCapability").get<std::string>();
    result.evidence = value.at("evidence").get<std::string>();
    result.partialEffects = value.at("partialEffects").get<std::string>();
    result.nextStep = value.at("nextStep").get<std::string>();
    result.implementationHints = value.at("implementationHints").get<std::string>();
    result.retestConditions = value.at("retestConditions").get<std::string>();
    result.owner = value.at("owner").get<std::string>();
    result.priority = value.at("priority").get<std::uint32_t>();
    result.dependencies = value.at("dependencies").get<std::map<std::string, std::string>>();
    if (!ValidGap(result)) throw std::runtime_error("Invalid bounded capability gap.");
    return result;
}

std::optional<std::map<std::string, std::string>> RelevantDependencies(const CapabilityGapObservation& gap,
    const std::map<std::string, std::string>& current)
{
    std::map<std::string, std::string> relevant;
    for (const auto& [name, ignored] : gap.dependencies)
    {
        const auto found = current.find(name);
        if (found == current.end() || found->second.empty() || found->second.size() > 256) return std::nullopt;
        relevant[name] = found->second;
    }
    return relevant;
}

bool RetestEligible(const SelfImprovementTask& task, const std::map<std::string, std::string>& current, const bool ownerRequested)
{
    if (!task.gap || task.gapAttempts.empty() || task.gapAttempts.size() >= 3 || task.gapAttempts.back().verifiedAcceptance) return false;
    const auto relevant = RelevantDependencies(*task.gap, current);
    return relevant && (ownerRequested || *relevant != task.gapAttempts.back().dependencies);
}

// Histories written before these categories were named this way. Translated on load so
// an older record still guards the problem it describes rather than being restored as
// an open task nothing recognises -- which is the same duplicate by another route.
std::string CanonicalCategory(std::string stored)
{
    if (stored == "performance") return ConversationLatencyCategory;
    if (stored == "voice") return FirstAudioLatencyCategory;
    if (stored == "reliability") return RuntimeReliabilityCategory;
    return stored;
}

template<typename T, typename Predicate>
bool ReadHistoryField(const json& record, const char* name, T& destination, Predicate accepts, const bool required = false)
{
    const auto value = record.find(name);
    if (value == record.end()) return !required;
    // Missing optional fields retain defaults. Explicit null or an incompatible
    // representation is malformed, not an instruction to silently reset a field.
    if (!accepts(*value)) return false;
    destination = value->get<T>();
    return true;
}

bool DecodeHistoryRecord(const json& record, SelfImprovementTask& task, bool& resolved)
{
    if (!record.is_object()) return false;
    const auto isString = [](const json& value) { return value.is_string(); };
    const auto isBool = [](const json& value) { return value.is_boolean(); };
    const auto isNumber = [](const json& value)
    {
        return value.is_number() && std::isfinite(value.get<double>());
    };
    const auto isStrings = [](const json& value)
    {
        return value.is_array() && std::all_of(value.begin(), value.end(),
            [](const json& item) { return item.is_string(); });
    };
    if (!ReadHistoryField(record, "id", task.id, isString, true) || task.id.empty() ||
        !ReadHistoryField(record, "resolved", resolved, isBool)) return false;

    // Open tasks require identity, category and a problem. Resolution rows only
    // require identity and resolved=true, but every supplied known field is checked
    // before the resolution is allowed to affect recovered state.
    if (!ReadHistoryField(record, "category", task.category, isString, !resolved) ||
        !ReadHistoryField(record, "observedProblem", task.observedProblem, isString, !resolved) ||
        (!resolved && (task.category.empty() || task.observedProblem.empty())) ||
        !ReadHistoryField(record, "evidence", task.evidence, isString) ||
        !ReadHistoryField(record, "confidence", task.confidence, isNumber) ||
        !ReadHistoryField(record, "expectedBenefit", task.expectedBenefit, isNumber) ||
        !ReadHistoryField(record, "estimatedRisk", task.estimatedRisk, isNumber) ||
        !ReadHistoryField(record, "relatedMetrics", task.relatedMetrics, isStrings) ||
        !ReadHistoryField(record, "relatedComponents", task.relatedComponents, isStrings) ||
        !ReadHistoryField(record, "researchAllowed", task.researchAllowed, isBool) ||
        !ReadHistoryField(record, "researchCompleted", task.researchCompleted, isBool)) return false;
    task.category = CanonicalCategory(std::move(task.category));
    if (record.contains("gap"))
    {
        task.gap = DecodeGap(record.at("gap"));
        const auto& attempts = record.at("gapAttempts");
        if (!attempts.is_array() || attempts.empty() || attempts.size() > 3) return false;
        for (const auto& attempt : attempts)
        {
            CapabilityGapAttempt decoded{attempt.at("dependencies").get<std::map<std::string, std::string>>(),
                attempt.at("verifiedAcceptance").get<bool>(), attempt.at("evidence").get<std::string>()};
            const auto relevant = RelevantDependencies(*task.gap, decoded.dependencies);
            if (!relevant || *relevant != decoded.dependencies || decoded.evidence.empty() || decoded.evidence.size() > 4096)
                return false;
            task.gapAttempts.push_back(std::move(decoded));
        }
        if (resolved != task.gapAttempts.back().verifiedAcceptance) return false;
    }
    else if (record.contains("gapAttempts")) return false;
    return true;
}
}

bool SelfAssessmentEngine::Initialize(const std::filesystem::path& historyPath, std::string& outError)
{
    std::lock_guard lock(mutex);
    if (historyPath.empty())
    {
        outError = "The self-improvement history path is empty.";
        return false;
    }
    std::error_code error;
    if (!historyPath.parent_path().empty())
        std::filesystem::create_directories(historyPath.parent_path(), error);
    if (error)
    {
        outError = "Could not create self-improvement history: " + error.message();
        return false;
    }

    // Reading the history back is what makes it a history rather than an append-only
    // sink. Without this the category guards reset with the process, so every restart
    // could raise a fresh task for a problem already recorded -- which is the one thing
    // the file existed to prevent.
    const bool exists = std::filesystem::exists(historyPath, error);
    if (error || (exists && !std::filesystem::is_regular_file(historyPath, error)))
    {
        outError = "Could not read self-improvement history: " +
            (error ? error.message() : std::string("the history path is not a regular file."));
        return false;
    }
    std::ifstream file;
    if (exists)
    {
        file.open(historyPath);
        if (!file.is_open())
        {
            outError = "The self-improvement history could not be opened for reading.";
            return false;
        }
    }

    std::string line;
    std::unordered_map<std::string, SelfImprovementTask> byId;
    std::vector<std::string> order;
    std::unordered_set<std::string> resolved;
    std::size_t recoveredMalformed = 0;
    while (exists && std::getline(file, line))
    {
        if (line.empty()) continue;
        // Parsed per line, so a partially written final record -- the shape a crash
        // leaves behind -- costs that record and nothing before it.
        SelfImprovementTask task;
        bool isResolved = false;
        bool valid = false;
        try
        {
            const json record = json::parse(line, nullptr, false);
            valid = DecodeHistoryRecord(record, task, isResolved);
        }
        catch (const json::exception&)
        {
            // Keep the per-record boundary even if decoding later grows a typed read.
            // No malformed record may abort recovery or partially replace a task.
        }
        if (!valid)
        {
            ++recoveredMalformed;
            continue;
        }
        const std::string id = task.id;
        if (isResolved)
        {
            resolved.insert(id);
            continue;
        }
        if (byId.find(id) == byId.end()) order.push_back(id);
        byId[id] = std::move(task);
    }
    if (file.bad() || (exists && file.fail() && !file.eof()))
    {
        outError = "The self-improvement history could not be completely read.";
        return false;
    }

    std::vector<SelfImprovementTask> recoveredTasks;
    std::unordered_set<std::string> recoveredCategories;
    for (const std::string& id : order)
    {
        if (resolved.count(id) != 0) continue;
        const SelfImprovementTask& task = byId[id];
        recoveredTasks.push_back(task);
        // The guard follows what is still open, not what this process created. That is
        // the difference between "already raised" and "raised since the last restart".
        // Storing the category itself means a kind that gains a task later is guarded
        // without anyone having to remember to add a case here.
        if (!task.category.empty()) recoveredCategories.insert(task.category);
    }

    // A missing journal is a valid empty first run. Read failures above preserve the
    // previous snapshot, category guards, diagnostics and persistence destination.
    path = historyPath;
    snapshot.openTasks.swap(recoveredTasks);
    openCategories.swap(recoveredCategories);
    malformedHistoryRecords = recoveredMalformed;
    lastPersistenceError.clear();
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
        if (retired.gap)
        {
            outError = "A capability gap closes only after its recorded acceptance check.";
            return false;
        }
    }

    // Written first, and only then forgotten.
    //
    // The order used to be the other way round, and the failure it produced was the
    // worst kind of quiet: a resolution that could not be written still emptied the
    // task out of this process, while the record saying it was open stayed on disk. The
    // task vanished from the panel and came back at the next start, and nothing in
    // between said why. Appended rather than rewritten, so the history stays
    // append-only and a resolution is simply another record the loader honours.
    if (!PersistResolution(retired, outError)) return false;

    std::lock_guard lock(mutex);
    const auto found = std::find_if(
        snapshot.openTasks.begin(), snapshot.openTasks.end(),
        [&taskId](const SelfImprovementTask& task) { return task.id == taskId; });
    if (found != snapshot.openTasks.end()) snapshot.openTasks.erase(found);
    // The guard goes with it: the problem is no longer recorded as open, so crossing
    // the threshold again may legitimately raise it again.
    if (!retired.category.empty()) openCategories.erase(retired.category);
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
    // Decide, write, then record.
    //
    // A task used to be pushed into the snapshot and its guard raised before anything
    // was written, so a task the panel listed as recorded could be one the next start
    // had never heard of. The three phases below keep the claim and the disk together:
    // nothing enters the snapshot until its record is on disk, and a category reserved
    // for a write that failed is released again so the problem can still be raised
    // later. (The conclusion was also being appended to outside the lock, which is a
    // data race against any concurrent reader; it is now written under it.)
    std::vector<SelfImprovementTask> candidates;
    {
        std::lock_guard lock(mutex);
        const auto reserve = [this](const char* category)
        {
            return openCategories.insert(category).second;
        };

        if (snapshot.conversationTurns >= 8 &&
            snapshot.slowTurns * 4 >= snapshot.conversationTurns &&
            reserve(ConversationLatencyCategory))
        {
            SelfImprovementTask task;
            task.id = actions::NewActionId();
            task.category = ConversationLatencyCategory;
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
            candidates.push_back(std::move(task));
        }
        if (snapshot.voiceStalls >= 5 && reserve(FirstAudioLatencyCategory))
        {
            SelfImprovementTask task;
            task.id = actions::NewActionId();
            task.category = FirstAudioLatencyCategory;
            task.observedProblem = "Voice phrase generation repeatedly exceeds 12 seconds.";
            task.evidence = std::to_string(snapshot.voiceStalls) +
                " generated phrases or voice errors crossed the stall threshold.";
            task.confidence = 0.86;
            task.expectedBenefit = 0.8;
            task.estimatedRisk = 0.25;
            task.relatedMetrics = {"qwen_synthesis", "queue_depth", "device"};
            task.relatedComponents = {"Qwen3-TTS", "ReplyFragmenter", "Resource planner"};
            task.researchAllowed = true;
            candidates.push_back(std::move(task));
        }
        const std::uint64_t reliabilityFailures =
            snapshot.internetFailures + snapshot.memoryFailures;
        if (reliabilityFailures >= 5 && reserve(RuntimeReliabilityCategory))
        {
            SelfImprovementTask task;
            task.id = actions::NewActionId();
            task.category = RuntimeReliabilityCategory;
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
            candidates.push_back(std::move(task));
        }
    }

    // File work, deliberately outside the lock: a slow or blocked disk must not hold up
    // Observe() on the runtime event thread.
    std::vector<SelfImprovementTask> recorded;
    std::vector<SelfImprovementTask> lost;
    std::string writeError;
    for (SelfImprovementTask& task : candidates)
    {
        std::string error;
        if (PersistTask(task, error)) recorded.push_back(std::move(task));
        else
        {
            if (writeError.empty()) writeError = error;
            lost.push_back(std::move(task));
        }
    }

    std::lock_guard lock(mutex);
    for (SelfImprovementTask& task : recorded)
    {
        snapshot.openTasks.push_back(std::move(task));
    }
    for (const SelfImprovementTask& task : lost)
    {
        // Nothing was written, so nothing is guarded. The next Assess() may try again.
        openCategories.erase(task.category);
    }
    snapshot.conclusion = snapshot.openTasks.empty()
        ? snapshot.conversationTurns < 8
            ? "Not enough evidence yet. No change recommended."
            : "No measured threshold is currently crossed. No change recommended."
        : std::to_string(snapshot.openTasks.size()) +
            " evidence-backed improvement task(s) are ready for review.";
    if (!lost.empty())
    {
        // Said out loud rather than shown as a recorded task: the problem was observed
        // and could not be written down, and those are different states.
        snapshot.conclusion += " (" + std::to_string(lost.size()) +
            " self-improvement task(s) could not be saved and are not recorded: " +
            writeError + ")";
    }
    return snapshot;
}

SelfAssessmentSnapshot SelfAssessmentEngine::Snapshot() const
{
    std::lock_guard lock(mutex);
    return snapshot;
}

bool SelfAssessmentEngine::AppendRecord(const json& record, std::string& outError, const std::function<bool()>& admission)
{
    const std::lock_guard persistenceLock(persistenceMutex);
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
    if (admission && !admission())
    {
        outError = "The judged quality origin is no longer admitted.";
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
    file.close();
    if (!file.good())
    {
        outError = "The self-improvement history could not be written: " +
            destination.string();
        return false;
    }
    return true;
}

bool SelfAssessmentEngine::PersistTask(
    const SelfImprovementTask& task, std::string& outError, const bool resolved, const std::function<bool()>& admission)
{
    json record = {
        {"id", task.id}, {"category", task.category},
        {"observedProblem", task.observedProblem}, {"evidence", task.evidence},
        {"confidence", task.confidence}, {"expectedBenefit", task.expectedBenefit},
        {"estimatedRisk", task.estimatedRisk}, {"relatedMetrics", task.relatedMetrics},
        {"relatedComponents", task.relatedComponents},
        {"researchAllowed", task.researchAllowed},
        {"researchCompleted", task.researchCompleted},
        {"resolved", resolved}
    };
    if (task.gap)
    {
        record["gap"] = GapJson(*task.gap);
        record["gapAttempts"] = json::array();
        for (const auto& attempt : task.gapAttempts)
            record["gapAttempts"].push_back({{"dependencies", attempt.dependencies}, {"verifiedAcceptance", attempt.verifiedAcceptance},
                {"evidence", attempt.evidence}});
    }
    if (AppendRecord(record, outError, admission))
        return true;
    std::lock_guard lock(mutex);
    lastPersistenceError = outError;
    return false;
}

namespace
{
std::map<std::string, std::string> JudgedRetestDependencies(const QualityFeedback& feedback)
{
    if (!feedback.dependencies.contains("loaded-profile-sha256") || !feedback.dependencies.contains("effective-llm-config-sha256"))
        return feedback.dependencies;
    std::map<std::string, std::string> dependencies;
    for (const auto* name : {"loaded-profile-sha256", "effective-llm-config-sha256", "native-answer-contract"})
    {
        if (const auto found = feedback.dependencies.find(name); found != feedback.dependencies.end())
            dependencies.emplace(*found);
    }
    return dependencies;
}
}

bool SelfAssessmentEngine::RecordJudgedQualityFailure(
    const QualityFeedback& feedback, std::string& id, std::string& error, std::function<bool()> admission)
{
    id.clear();
    if (!ValidateQualityFeedback(feedback, error) || feedback.criterionSatisfied)
    {
        if (error.empty())
            error = "Only an explicit failed criterion creates a quality gap.";
        return false;
    }
    CapabilityGapObservation observation;
    observation.goal = QualityCriterionKey(feedback);
    observation.attempts = {"Exact " + ToString(feedback.target) + " judged against a supplied acceptance criterion."};
    observation.failures = {"The judged " + ToString(feedback.issue) + " criterion was not satisfied."};
    observation.reason = CapabilityGapReason::Unknown;
    observation.cause = "The failed criterion is observed; its general cause is not established.";
    observation.evidence = QualityFeedbackEvidence(feedback);
    observation.partialEffects = "The cited result was delivered; this judgment changes no output, settings or personality.";
    observation.nextStep = "Review a changed candidate against the recorded criterion and fresh exact-result evidence.";
    observation.implementationHints = feedback.criterion;
    observation.retestConditions = "Relevant dependency change or bounded explicit owner retest, followed by a fresh criterion judgment.";
    observation.owner = feedback.target == QualityTarget::ConversationTurn ? "Conversation" : "Agent Studio";
    observation.priority = 1;
    observation.dependencies = JudgedRetestDependencies(feedback);
    return RecordGap(observation, id, error, std::move(admission));
}

bool SelfAssessmentEngine::RecordJudgedQualityRetest(
    const std::string& id, const QualityFeedback& feedback, std::string& error, const bool ownerRequested, std::function<bool()> admission)
{
    if (!ValidateQualityFeedback(feedback, error))
        return false;
    const std::lock_guard operationLock(gapMutex);
    const auto current = Snapshot();
    const auto task = std::find_if(current.openTasks.begin(), current.openTasks.end(), [&](const auto& value) { return value.id == id; });
    const std::string evidence = QualityFeedbackEvidence(feedback);
    if (task == current.openTasks.end() || !task->gap || task->gap->goal != QualityCriterionKey(feedback) ||
        std::any_of(task->gapAttempts.begin(), task->gapAttempts.end(),
            [&](const auto& value)
            {
                return value.evidence == evidence ||
                       (feedback.criterionSatisfied && value.evidence.find("target SHA256 " + feedback.targetDigest) != std::string::npos);
            }))
    {
        error = "Quality retest requires the recorded criterion and fresh exact-result judgment.";
        return false;
    }
    return RecordGapAttemptLocked(
        id, JudgedRetestDependencies(feedback), ownerRequested, feedback.criterionSatisfied, evidence, error, std::move(admission));
}

bool SelfAssessmentEngine::RecordGap(
    const CapabilityGapObservation& observation, std::string& id, std::string& error, std::function<bool()> admission)
{
    const std::lock_guard operationLock(gapMutex);
    if (!ValidGap(observation) || (admission && !admission()))
    {
        error = "Capability gap lacks bounded admitted evidence or dependencies.";
        return false;
    }
    {
        const std::lock_guard lock(mutex);
        if (snapshot.openTasks.size() >= 256) { error = "Self-assessment task capacity is exhausted."; return false; }
        for (const auto& task : snapshot.openTasks)
            if (task.gap && task.gap->goal == observation.goal && task.gap->missingCapability == observation.missingCapability &&
                task.gap->reason == observation.reason) { id = task.id; error.clear(); return true; }
    }
    SelfImprovementTask task;
    task.id = actions::NewActionId();
    task.category = "capability_gap:" + task.id;
    task.observedProblem = observation.reason == CapabilityGapReason::MissingCapability ? observation.missingCapability : observation.failures.front();
    task.evidence = observation.evidence;
    task.relatedComponents = {observation.owner == "Conversation" || observation.owner == "Agent Studio" ? observation.owner : "Skills"};
    task.gap = observation;
    task.gapAttempts.push_back({observation.dependencies, false, observation.evidence});
    if (!PersistTask(task, error, false, admission))
        return false;
    const std::lock_guard lock(mutex);
    openCategories.insert(task.category);
    snapshot.openTasks.push_back(task);
    id = task.id;
    error.clear();
    return true;
}

bool SelfAssessmentEngine::CanRetest(const std::string& id, const std::map<std::string, std::string>& dependencies,
    const bool ownerRequested) const
{
    const std::lock_guard lock(mutex);
    for (const auto& task : snapshot.openTasks) if (task.id == id) return RetestEligible(task, dependencies, ownerRequested);
    return false;
}

bool SelfAssessmentEngine::RecordGapAttempt(const std::string& id, const std::map<std::string, std::string>& dependencies,
    const bool ownerRequested, const bool verified, const std::string& evidence, std::string& error, std::function<bool()> admission)
{
    const std::lock_guard operationLock(gapMutex);
    return RecordGapAttemptLocked(id, dependencies, ownerRequested, verified, evidence, error, std::move(admission));
}

bool SelfAssessmentEngine::RecordGapAttemptLocked(const std::string& id, const std::map<std::string, std::string>& dependencies,
    const bool ownerRequested, const bool verified, const std::string& evidence, std::string& error, std::function<bool()> admission)
{
    SelfImprovementTask updated;
    {
        const std::lock_guard lock(mutex);
        const auto found = std::find_if(snapshot.openTasks.begin(), snapshot.openTasks.end(), [&](const auto& task) { return task.id == id; });
        if (found == snapshot.openTasks.end() || !RetestEligible(*found, dependencies, ownerRequested) || evidence.empty() ||
            evidence.size() > 4096 || memory::ContainsSensitiveContent(evidence))
        {
            error = "Gap retest lacks relevant change, bounded request or acceptance evidence.";
            return false;
        }
        updated = *found;
    }
    updated.gapAttempts.push_back({*RelevantDependencies(*updated.gap, dependencies), verified, evidence});
    if (!PersistTask(updated, error, verified, admission))
        return false;
    const std::lock_guard lock(mutex);
    const auto found = std::find_if(snapshot.openTasks.begin(), snapshot.openTasks.end(), [&](const auto& task) { return task.id == id; });
    if (found != snapshot.openTasks.end())
    {
        if (verified) { openCategories.erase(found->category); snapshot.openTasks.erase(found); }
        else *found = std::move(updated);
    }
    error.clear();
    return true;
}

bool SelfAssessmentEngine::PersistResolution(const SelfImprovementTask& task, std::string& outError)
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
