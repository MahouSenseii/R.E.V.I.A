#include "reviaSessionTestAccess.h"
#include "testSupport.h"

#include "Audit/contentDigest.h"
#include "Memory/longTermMemory.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>

namespace
{
using namespace revia::runtime;
using revia::tests::Check;

revia::learning::QualityFeedback Feedback(ReviaSession& session)
{
    revia::learning::QualityFeedback value;
    value.origin = session.Stamp();
    value.audienceRevision = session.Audience().revision;
    value.judgmentId = "explicit-owner-judgment-1";
    value.sourceId = "owner-review:synthetic-displayed-reply";
    value.targetId = "displayed-reply:synthetic-exact-receipt";
    value.targetDigest = revia::audit::ContentDigest("Actual synthetic displayed reply");
    value.criterion = "Include the explicitly requested public object-file fact.";
    value.evidence = "The owner selected this displayed reply and judged the requested fact absent.";
    value.dependencies = {{"displayed-result", value.targetDigest}, {"answer-contract", "synthetic-v1"}};
    return value;
}

void Prepare(ReviaSession& session, const std::filesystem::path& root)
{
    std::string error;
    Check(ReviaSessionTestAccess::InitializeQualityAssessment(session, root / "assessment.jsonl", error), error);
    ReviaSessionTestAccess::DisableQualityFixtureEmbedding(session);
    Check(session.SetAudience({revia::identity::AudienceKind::Private, "fixture-private", 0, {}}, error), error);
}

void TestRuntimeJudgmentAuthorityAndAudienceEpoch()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"quality-bridge", "Synthetic quality bridge", "assistant", false});
    ReviaSession session(paths);
    Prepare(session, fixture.root);
    longTermMemory memory(paths.Resolve("Memory/revia_memory.db").string());
    const auto original = Feedback(session);
    std::string error, taskId, recordId;
    auto invalid = original;
    ++invalid.origin.generation;
    Check(!session.RecordQualityFeedback(invalid, taskId, recordId, error), "Stale quality origin entered the native bridge.");
    invalid = original;
    invalid.source = revia::learning::QualityEvidenceSource::DeterministicAcceptance;
    Check(!session.RecordQualityFeedback(invalid, taskId, recordId, error), "Caller forged native acceptance authority.");
    invalid = original;
    invalid.targetDigest = "transport-success";
    Check(!session.RecordQualityFeedback(invalid, taskId, recordId, error), "Transport success served as an exact target digest.");
    invalid = original;
    invalid.targetId.clear();
    Check(!session.RecordQualityFeedback(invalid, taskId, recordId, error), "An absent displayed target entered judged learning.");
    invalid = original;
    invalid.privateContextExcluded = true;
    Check(!session.RecordQualityFeedback(invalid, taskId, recordId, error), "Excluded private context entered the runtime candidate.");
    invalid = original;
    invalid.evidence = "password=synthetic-secret-credential";
    Check(!session.RecordQualityFeedback(invalid, taskId, recordId, error), "A secret entered runtime feedback evidence.");
    Check(session.SetAudience({revia::identity::AudienceKind::Public, "fixture-public", 0, {}}, error), error);
    invalid = Feedback(session);
    Check(!session.RecordQualityFeedback(invalid, taskId, recordId, error), "Public feedback entered private learned history.");
    Check(session.SetAudience({revia::identity::AudienceKind::Private, "fixture-private", 0, {}}, error), error);
    Check(!session.RecordQualityFeedback(original, taskId, recordId, error),
        "Private-to-public-to-private audience changes reauthorized an old displayed judgment.");
    Check(memory.Load().empty() && session.LearningStudio().lessons.empty(),
        "Refused runtime judgment produced a candidate or trusted memory.");
}

void TestPrivateCandidateCapturedConfigurationAndOrdinaryReview()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"quality-review", "Synthetic private review", "assistant", false});
    ReviaSession session(paths);
    Prepare(session, fixture.root);
    longTermMemory memory(paths.Resolve("Memory/revia_memory.db").string());
    auto value = Feedback(session);
    value.dependencies["loaded-profile-sha256"] = "forged-profile";
    value.dependencies["effective-llm-config-sha256"] = "forged-configuration";
    std::string error, taskId, recordId;
    Check(session.RecordQualityFeedback(value, taskId, recordId, error), error);
    const auto studio = session.LearningStudio();
    const auto record = std::find_if(studio.lessons.begin(), studio.lessons.end(), [&](const auto& item) { return item.id == recordId; });
    Check(record != studio.lessons.end() && record->disposition == revia::learning::LearningDisposition::PrivateCandidate &&
              memory.Load().empty(),
        "Native owner judgment bypassed ordinary pending learning review.");
    Check(record->candidate.evidence.sources.front() == value.sourceId && record->candidate.evidence.sources.at(1) == value.targetId &&
              record->candidate.lesson.evidence.find(value.targetDigest) != std::string::npos &&
              record->candidate.lesson.evidence.find(value.evidence) != std::string::npos,
        "The runtime changed the owner's exact source, target digest or evidence.");
    const auto& conditions = record->candidate.evidence.conditions;
    Check(std::any_of(conditions.begin(), conditions.end(), [](const auto& text)
              { return text.starts_with("Dependency loaded-profile-sha256: ") && text.find("forged-profile") == std::string::npos; }) &&
              std::any_of(conditions.begin(), conditions.end(),
                  [](const auto& text)
                  {
                      return text.starts_with("Dependency effective-llm-config-sha256: ") &&
                             text.find("forged-configuration") == std::string::npos;
                  }),
        "Caller-controlled dependencies displaced native captured configuration.");
    value.judgmentId = "explicit-owner-judgment-2";
    value.targetId = "displayed-reply:fresh-unchanged-configuration";
    value.targetDigest = revia::audit::ContentDigest("Fresh result under unchanged native configuration");
    value.dependencies["displayed-result"] = value.targetDigest;
    Check(!session.RetestQualityFeedback(taskId, value, error), "New displayed output counted as changed native configuration.");
    ReviaSessionTestAccess::PlannedMainDevice(session, "synthetic-new-device");
    Check(session.RetestQualityFeedback(taskId, value, error), error);
    value.judgmentId = "explicit-owner-relabel-same-failed-result";
    value.evidence = "The same failed retest was relabeled satisfactory without a new result.";
    value.criterionSatisfied = true;
    Check(!session.RetestQualityFeedback(taskId, value, error, true),
        "Runtime owner judgment resolved the task using an already-failed retest result.");
    value.judgmentId = "explicit-owner-judgment-3";
    value.targetId = "displayed-reply:fresh-held-out-acceptance";
    value.targetDigest = revia::audit::ContentDigest("Fresh held-out reply contains the requested public fact");
    value.evidence = "The owner checked the fresh held-out reply and found the requested fact present.";
    value.criterionSatisfied = true;
    Check(session.RetestQualityFeedback(taskId, value, error, true), error);
    Check(memory.Load().empty(), "Criterion satisfaction automatically inserted trusted memory.");
    Check(session.ReviewLearning(
              recordId, revia::learning::LearningDecision::Accept, "The owner accepts this exact scoped quality observation.", error),
        error);
    Check(memory.Load().size() == 1 && memory.Load().front().source == "reviewed_lesson:" + recordId &&
              memory.Load().front().category == "constraint",
        "Explicit ordinary learning review did not produce the actual quality-memory receipt.");
}

void TestRuntimeMemorySelectionAndAudienceEpoch()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"revision-bridge", "Synthetic revision bridge", "assistant", false});
    ReviaSession session(paths);
    Prepare(session, fixture.root);
    longTermMemory memory(paths.Resolve("Memory/revia_memory.db").string());
    memoryDecision original;
    original.bSuccess = original.bShouldRemember = true;
    original.category = "project";
    original.summary = "The owner tracks Amber at eight.";
    bool added;
    std::string originalId, error;
    Check(memory.Save(original, added, &originalId), "Runtime revision original fixture save failed.");
    revia::memory::MemoryRevisionRequest request;
    request.ownerRequestId = "owner-selected-revision-1";
    request.originalId = originalId;
    request.expectedSummaryDigest = revia::audit::ContentDigest(original.summary);
    request.origin = session.Stamp();
    request.audienceRevision = session.Audience().revision;
    request.corrected = original;
    request.corrected.summary = "The owner tracks Amber at nine.";
    request.reason = "The owner selected this exact retained record and requested replacement.";
    request.evidence = "Explicit selected-row replacement supplied by the owner.";
    revia::memory::MemoryRevisionReceipt receipt;
    auto invalid = request;
    ++invalid.origin.generation;
    Check(!session.ReviseMemoryOwnerRequested(invalid, receipt, error), "Stale origin revised native memory.");
    invalid = request;
    invalid.expectedSummaryDigest = revia::audit::ContentDigest("Unselected different text");
    Check(!session.ReviseMemoryOwnerRequested(invalid, receipt, error), "Unselected digest revised native memory.");
    Check(session.SetAudience({revia::identity::AudienceKind::Public, "fixture-public", 0, {}}, error), error);
    invalid = request;
    invalid.audienceRevision = session.Audience().revision;
    Check(!session.ReviseMemoryOwnerRequested(invalid, receipt, error), "Public context revised private memory.");
    Check(session.SetAudience({revia::identity::AudienceKind::Private, "fixture-private", 0, {}}, error), error);
    Check(!session.ReviseMemoryOwnerRequested(request, receipt, error), "A new audience epoch reauthorized old memory selection.");
    Check(memory.Load().size() == 1 && memory.RevisionHistory().empty(), "Refused bridge revision changed storage.");
    request.audienceRevision = session.Audience().revision;
    Check(session.ReviseMemoryOwnerRequested(request, receipt, error), error);
    Check(receipt.originalId == originalId && receipt.audienceRevision == request.audienceRevision && memory.Load().size() == 2 &&
              memory.RevisionHistory().size() == 1,
        "Exact private owner selection lost its atomic revision or audience receipt.");
}

void TestCanceledQualityMutationsDoNotWriteAfterForegroundWait()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"quality-cancellation", "Synthetic cancellation", "assistant", false});
    ReviaSession session(paths);
    Prepare(session, fixture.root);
    longTermMemory memory(paths.Resolve("Memory/revia_memory.db").string());
    memoryDecision original;
    original.bSuccess = original.bShouldRemember = true;
    original.category = "project";
    original.summary = "The owner tracks Amber at eight.";
    bool added;
    std::string originalId, error, taskId, recordId;
    Check(memory.Save(original, added, &originalId), "Cancellation fixture original save failed.");
    const auto feedback = Feedback(session);
    revia::memory::MemoryRevisionRequest revision;
    revision.ownerRequestId = "owner-canceled-revision";
    revision.originalId = originalId;
    revision.expectedSummaryDigest = revia::audit::ContentDigest(original.summary);
    revision.origin = session.Stamp();
    revision.audienceRevision = session.Audience().revision;
    revision.corrected = original;
    revision.corrected.summary = "The owner tracks Amber at nine.";
    revision.reason = "Explicit exact owner replacement.";
    revision.evidence = "Selected original and supplied replacement.";
    revia::memory::MemoryRevisionReceipt receipt;
    std::stop_source canceled;
    canceled.request_stop();
    Check(!session.RecordQualityFeedback(feedback, taskId, recordId, error, canceled.get_token()),
        "Already-canceled owner review wrote judged quality state.");
    Check(!session.ReviseMemoryOwnerRequested(revision, receipt, error, canceled.get_token()),
        "Already-canceled owner revision changed memory.");
    for (const bool reviseMemory : {false, true})
    {
        std::stop_source stop;
        auto foreground = ReviaSessionTestAccess::HoldForeground(session);
        std::promise<void> launched;
        auto entered = launched.get_future();
        auto mutation = std::async(std::launch::async,
            [&]
            {
                launched.set_value();
                if (reviseMemory)
                {
                    return session.ReviseMemoryOwnerRequested(revision, receipt, error, stop.get_token());
                }
                return session.RecordQualityFeedback(feedback, taskId, recordId, error, stop.get_token());
            });
        const bool held = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
                          mutation.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout;
        stop.request_stop();
        foreground.unlock();
        const bool changed = mutation.get();
        Check(held, "Cancellation fixture did not hold the actual foreground mutation lock.");
        Check(!changed, "Canceled mutation resumed and wrote after foreground lock release.");
    }
    Check(taskId.empty() && recordId.empty() && receipt.requestId.empty() && !std::filesystem::exists(fixture.root / "assessment.jsonl") &&
              session.LearningStudio().lessons.empty() && memory.Load().size() == 1 && memory.RevisionHistory().empty(),
        "Canceled review/revision left a task, candidate, corrected memory or receipt.");
}

void TestCanceledQualityRetestPreservesAttemptHistory()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"quality-retest-cancellation", "Synthetic retest cancellation", "assistant", false});
    ReviaSession session(paths);
    Prepare(session, fixture.root);
    auto feedback = Feedback(session);
    std::string error, taskId, recordId;
    Check(session.RecordQualityFeedback(feedback, taskId, recordId, error), error);
    feedback.judgmentId = "canceled-retest-judgment";
    feedback.targetId = "displayed-reply:canceled-fresh-held-out-result";
    feedback.targetDigest = revia::audit::ContentDigest("Fresh held-out result judged satisfactory");
    feedback.evidence = "The owner checked the fresh held-out result against the recorded criterion.";
    feedback.criterionSatisfied = true;
    std::stop_source canceled;
    canceled.request_stop();
    Check(!session.RetestQualityFeedback(taskId, feedback, error, true, canceled.get_token()),
        "Already-canceled held-out judgment resolved a quality gap.");
    std::stop_source stop;
    auto foreground = ReviaSessionTestAccess::HoldForeground(session);
    std::promise<void> launched;
    auto entered = launched.get_future();
    auto retest = std::async(std::launch::async,
        [&]
        {
            launched.set_value();
            return session.RetestQualityFeedback(taskId, feedback, error, true, stop.get_token());
        });
    const bool held = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
                      retest.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout;
    stop.request_stop();
    foreground.unlock();
    const bool changed = retest.get();
    Check(held && !changed, "Canceled held-out judgment resolved a gap after foreground lock release.");
    revia::learning::SelfAssessmentEngine recovered;
    Check(recovered.Initialize(fixture.root / "assessment.jsonl", error), error);
    Check(recovered.Snapshot().openTasks.size() == 1 && recovered.Snapshot().openTasks.front().gapAttempts.size() == 1,
        "Canceled retest wrote an attempt or resolution to the durable quality history.");
}
}

void RunQualityFeedbackRuntimeTests()
{
    TestRuntimeJudgmentAuthorityAndAudienceEpoch();
    TestPrivateCandidateCapturedConfigurationAndOrdinaryReview();
    TestRuntimeMemorySelectionAndAudienceEpoch();
    TestCanceledQualityMutationsDoNotWriteAfterForegroundWait();
    TestCanceledQualityRetestPreservesAttemptHistory();
    std::cout << "Runtime quality feedback checks passed: owner authority, audience epochs, captured dependencies and ordinary reviewed "
                 "receipts.\n";
}
