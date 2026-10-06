#include "Runtime/reviaSession.h"

#include "Actions/actionTypes.h"
#include "Audit/contentDigest.h"
#include "Memory/sensitiveContent.h"

#include <algorithm>
#include <sstream>

namespace revia::runtime
{
namespace
{
constexpr const char* InventorySkill = "workspace-inventory";

std::vector<skills::SkillCase> FreshInventoryCases()
{
    return {{"fresh-empty", {}, "0 entries: 0 files, 0 directories, 0 links, 0 other."},
        {"fresh-nested", {"[DIR] projects", "[FILE] notes.txt", "[LINK] shortcut"},
            "3 entries: 1 files, 1 directories, 1 links, 0 other."}};
}
}

bool ReviaSession::InitializeLearningStudio(std::string& outError)
{
    if (!Admits(Stamp()))
    {
        outError = "This companion session is no longer active.";
        return false;
    }
    if (!skillPackages)
    {
        auto packages = std::make_unique<skills::SkillPackageStore>(companionPaths.Resolve("RuntimeData/Skills"),
            companionPaths.InstallRoot() / "Config/Skills");
        if (!packages->Initialize(outError))
            return false;
        skillPackages = std::move(packages);
    }
    if (!learningRecords)
    {
        auto records = std::make_unique<learning::LearningRecordStore>(companionPaths.Resolve("RuntimeData/Learning"),
            [this] { return Admits(Stamp()); });
        if (!records->Initialize(outError))
            return false;
        learningRecords = std::move(records);
    }
    return true;
}

bool ReviaSession::CollectLearningCandidates(std::string& outError)
{
    if (!InitializeLearningStudio(outError))
        return false;
    for (const auto& lesson : DrawLessons())
    {
        learning::LearningCandidate candidate;
        candidate.lesson = lesson;
        candidate.origin = Stamp();
        candidate.evidence.sources = {lesson.kind == learning::LessonKind::Planning ? "companion goal outcomes" : "companion initiative decisions"};
        candidate.evidence.conditions = {"Observed aggregate; does not authorize actions or establish a universal rule."};
        candidate.evidence.supporting = {lesson.evidence};
        candidate.evidence.toolVersion = "runtime-metrics-v1";
        candidate.evidence.checkedAt = "observed in session " + candidate.origin.sessionId;
        std::string id;
        if (!learningRecords->Propose(candidate, id, outError))
            return false;
        const auto record = learningRecords->Find(id);
        if (!record)
            return false;
        if (record->decision == learning::LearningDecision::Accept || record->decision == learning::LearningDecision::Reject)
            continue;
        const learning::LearningChecks checks{record->digest,
            lesson.sampleSize >= learning::LearningReview::MinimumSamples && !lesson.statement.empty() && !lesson.evidence.empty(),
            !memory::ContainsSensitiveContent(lesson.statement + "\n" + lesson.evidence), true};
        if (!learningRecords->Check(id, checks, outError))
            return false;
    }
    return true;
}

LearningStudioSnapshot ReviaSession::LearningStudio()
{
    std::lock_guard lock(learningStudioMutex);
    LearningStudioSnapshot snapshot;
    if (!CollectLearningCandidates(snapshot.status))
        return snapshot;
    const auto pin = skillPackages->Pin(InventorySkill);
    if (pin)
        snapshot.inventorySkill = pin->reference;
    snapshot.lessons = learningRecords->History();
    snapshot.status = "Private candidates require verification and a review decision before memory admission.";
    return snapshot;
}

SessionResult ReviaSession::RunInventorySkill(const std::filesystem::path& directory, const std::stop_token stop)
{
    SessionResult result;
    result.succeeded = false;
    result.stamp = Stamp();
    std::optional<skills::SkillPin> pin;
    {
        std::lock_guard lock(learningStudioMutex);
        if (!InitializeLearningStudio(result.reason))
        {
            result.text = result.reason;
            return result;
        }
        pin = skillPackages->Pin(InventorySkill);
    }
    if (!pin || !skills::RequiresCapability(*pin, actions::ActionType::ListDirectory))
    {
        result.text = result.reason = "No verified workspace inventory package is active.";
        return result;
    }
    actions::ActionRequest request;
    request.id = actions::NewActionId();
    request.type = actions::ActionType::ListDirectory;
    request.source = directory;
    request.requestedBy = "skill:" + pin->reference.id + ":" + pin->reference.version;
    const auto outcome = actionRuntime.ExecuteFor(result.stamp, request, false, stop);
    if (!outcome.Succeeded() || stop.stop_requested() || !Admits(result.stamp))
    {
        result.text = result.reason = !Admits(result.stamp) ? "The inventory result belongs to an inactive session." : outcome.Message();
        return result;
    }
    const auto inventory = skills::SummarizeInventory(*pin, outcome.result.entries);
    result.succeeded = inventory.verified && inventory.complete;
    result.text = inventory.summary + "\nSkill " + pin->reference.id + " v" + pin->reference.version + " · SHA256 " + pin->reference.digest;
    if (!result.succeeded)
        result.reason = "The admitted result did not satisfy the pinned skill's output contract.";
    const std::map<std::string, std::string> dependencies{{"inventory-skill", pin->reference.digest}};
    if (result.succeeded)
    {
        std::lock_guard lock(learningStudioMutex);
        learning::LearningCandidate candidate;
        candidate.origin = result.stamp;
        candidate.lesson.id = "inventory:" + pin->reference.digest;
        candidate.lesson.kind = learning::LessonKind::Planning;
        candidate.lesson.statement = "The admitted inventory run used workspace-inventory v" + pin->reference.version +
            " and satisfied that exact version's output contract. This observation applies only to the checked run.";
        candidate.lesson.evidence = "Native directory outcome and pinned package SHA256 " + pin->reference.digest +
            "; summary SHA256 " + audit::ContentDigest(inventory.summary);
        candidate.lesson.sampleSize = 1;
        candidate.evidence.sources = {"verified native inventory outcome"};
        candidate.evidence.conditions = {"One observed run; no universal performance claim or additional capability grant."};
        candidate.evidence.supporting = {candidate.lesson.evidence};
        candidate.evidence.toolVersion = pin->reference.id + ":" + pin->reference.version;
        candidate.evidence.checkedAt = "observed in session " + result.stamp.sessionId;
        std::string recordId;
        std::string error;
        if (learningRecords->Propose(candidate, recordId, error))
        {
            const auto record = learningRecords->Find(recordId);
            if (record && record->decision != learning::LearningDecision::Accept && record->decision != learning::LearningDecision::Reject)
                (void)learningRecords->Check(recordId, {record->digest, true,
                    !memory::ContainsSensitiveContent(candidate.lesson.statement + candidate.lesson.evidence), true}, error);
            if (error.empty()) result.text += "\nPrivate learning candidate " + recordId + " awaits parent review.";
        }
        if (!error.empty()) result.reason = "Inventory completed; its private learning receipt failed: " + error;
        for (const auto& task : selfAssessment.Snapshot().openTasks)
        {
            if (task.gap && task.gap->owner == "Skills" && selfAssessment.CanRetest(task.id, dependencies))
                (void)selfAssessment.RecordGapAttempt(task.id, dependencies, false, true,
                    "Actual admitted inventory passed the newly pinned package output contract.", error);
        }
    }
    else
    {
        learning::CapabilityGapObservation gap;
        gap.goal = "Summarize an admitted directory inventory using the pinned neutral skill.";
        gap.attempts = {"Native ListDirectory completed; closed skill output validation failed."};
        gap.successes = {"The directory was read through the action runtime."};
        gap.failures = {"The exact skill output contract was not satisfied."};
        gap.reason = inventory.verified ? learning::CapabilityGapReason::BudgetExhausted : learning::CapabilityGapReason::MissingCapability;
        gap.cause = inventory.verified ? "The native directory output limit omitted entries."
            : "The pinned skill could not verify every admitted inventory entry.";
        gap.causeProven = true;
        gap.missingCapability = inventory.verified ? "" : "Closed inventory output support for the observed entry kinds.";
        gap.evidence = "Skill SHA256 " + pin->reference.digest + "; no private directory names retained.";
        gap.partialEffects = "Read-only inventory completed; no file was changed.";
        gap.nextStep = "Review a neutral skill revision against synthetic regression and fresh cases.";
        gap.retestConditions = "The inventory-skill digest changes, or a bounded owner retest is requested.";
        gap.owner = "Skills";
        gap.priority = 1;
        gap.dependencies = dependencies;
        std::string id, error;
        (void)selfAssessment.RecordGap(gap, id, error);
    }
    return result;
}

bool ReviaSession::UpdateInventorySkill(std::string& outError)
{
    std::lock_guard lock(learningStudioMutex);
    if (!InitializeLearningStudio(outError))
        return false;
    skills::SkillPackage package;
    if (!skills::LoadSkillPackage(companionPaths.InstallRoot() / "Config/Skills/workspace-inventory/1.1.0", package, outError))
        return false;
    const auto verification = skills::VerifySkillPackage(package, FreshInventoryCases());
    const skills::SkillExportReview review{package.reference.digest, verification.evidenceDigest, true, true, true};
    return skillPackages->Publish(package, verification, review, outError) && skillPackages->Activate(package.reference, outError);
}

bool ReviaSession::RollbackInventorySkill(std::string& outError)
{
    std::lock_guard lock(learningStudioMutex);
    return InitializeLearningStudio(outError) && skillPackages->Rollback(InventorySkill, outError);
}

bool ReviaSession::ExportInventorySkill(std::filesystem::path& outDirectory, std::string& outError)
{
    std::lock_guard lock(learningStudioMutex);
    if (!InitializeLearningStudio(outError)) return false;
    const auto pin = skillPackages->Pin(InventorySkill);
    if (!pin) { outError = "No verified inventory skill is selected."; return false; }
    outDirectory = companionPaths.Resolve("RuntimeData/Skills/exports") / pin->reference.id / pin->reference.version;
    return skillPackages->Export(pin->reference, outDirectory, outError);
}

bool ReviaSession::ReviewLearning(const std::string& recordId, const learning::LearningDecision decision,
    const std::string& feedback, std::string& outSummary)
{
    std::lock_guard lock(learningStudioMutex);
    const RuntimeStamp origin = Stamp();
    if (!InitializeLearningStudio(outSummary) || !Admits(origin))
        return false;
    const auto existing = learningRecords->Find(recordId);
    if (existing && existing->decision == learning::LearningDecision::Accept && decision == learning::LearningDecision::Accept)
    {
        if (existing->disposition == learning::LearningDisposition::TrustedMemory)
        {
            outSummary = "This exact accepted lesson already has a durable memory receipt.";
            return true;
        }
    }
    else if (!learningRecords->Decide(recordId, decision, feedback, outSummary))
        return false;
    if (decision != learning::LearningDecision::Accept)
    {
        outSummary = "Review recorded; this candidate remains outside trusted memory.";
        return true;
    }
    const auto record = learningRecords->PendingAdmission(recordId);
    if (!record || record->candidate.origin.companionId != origin.companionId || !Admits(origin))
    {
        outSummary = "This exact candidate is not eligible for memory admission.";
        return false;
    }
    memoryDecision finding;
    finding.bSuccess = finding.bShouldRemember = true;
    finding.category = learning::LearningReview::MemoryCategory(record->candidate.lesson);
    finding.summary = learning::LearningReview::MemorySummary(record->candidate.lesson);
    finding.reason = "Verified candidate accepted by parent review.";
    finding.source = "reviewed_lesson:" + record->id;
    finding.subject = {memory::MemorySubjectKind::Companion, origin.companionId};
    std::string memoryId;
    const auto disposition = turnCoordinator.SubmitLearnedFinding(router, finding, 0, &memoryId, [this, origin] { return Admits(origin); });
    const bool saved = disposition != agents::LearnedFindingResult::Failed && !memoryId.empty();
    std::string recordError;
    if (!learningRecords->RecordAdmission(recordId, saved, memoryId, recordError))
    {
        outSummary = saved ? "Content was saved; recording its review receipt failed. Retry can recover the existing row."
                           : "Memory admission failed and its review receipt could not be recorded.";
        return false;
    }
    outSummary = saved ? "Remembered the accepted lesson with its durable review receipt."
                       : "The accepted lesson could not be saved; it remains eligible for a bounded retry.";
    return saved;
}

bool ReviaSession::HandleSkillCommand(const std::string& input, SessionResult& result)
{
    if (input != "/skill" && input.rfind("/skill ", 0) != 0)
        return false;
    const std::string argument = input.size() > 7 ? input.substr(7) : "";
    if (argument.rfind("inventory ", 0) == 0)
    {
        std::stop_token stop;
        {
            std::lock_guard lock(cancellationMutex);
            stop = activeStopSource.get_token();
        }
        result = RunInventorySkill(actions::Utf8ToPath(argument.substr(10)), stop);
    }
    else if (argument == "update" || argument == "rollback")
    {
        result.succeeded = argument == "update" ? UpdateInventorySkill(result.reason) : RollbackInventorySkill(result.reason);
        result.text = result.succeeded ? "The inventory skill version selection was updated. Active tasks keep their captured version."
                                       : result.reason;
    }
    else
    {
        result.succeeded = true;
        result.text = "Usage: /skill inventory <approved folder> | /skill update | /skill rollback";
    }
    return true;
}
}
