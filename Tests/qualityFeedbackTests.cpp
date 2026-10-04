#include "testSupport.h"
#include "Audit/contentDigest.h"
#include "Learning/qualityFeedback.h"
#include "Learning/selfAssessment.h"

#include <iostream>

namespace
{
using namespace revia::learning;
using revia::tests::Check;

QualityFeedback Feedback()
{
    QualityFeedback value;
    value.origin = {"synthetic-companion", "synthetic-session", 1, {}, {}, 1};
    value.audienceRevision = 1;
    value.judgmentId = "owner-judgment-1";
    value.sourceId = "owner-review-1";
    value.targetId = "conversation-turn-1";
    value.targetDigest = revia::audit::ContentDigest("Actual synthetic delivered reply");
    value.criterion = "Answer the requested public object-file question.";
    value.evidence = "The displayed reply omitted the requested fact.";
    value.dependencies = {{"answer-contract", "v1"}, {"model", "synthetic-v1"}};
    return value;
}

void TestJudgedFailuresAndRelevantRetests()
{
    revia::tests::ScopedTestDirectory fixture;
    SelfAssessmentEngine owner;
    std::string error, id, repeated;
    auto value = Feedback();
    Check(owner.Initialize(fixture.root / "assessment.jsonl", error), error);
    Check(owner.RecordJudgedQualityFailure(value, id, error) && owner.RecordJudgedQualityFailure(value, repeated, error), error);
    Check(id == repeated && owner.Snapshot().openTasks.size() == 1, "Exact judged failure created duplicate work.");
    const auto task = owner.Snapshot().openTasks.front();
    Check(task.relatedComponents == std::vector<std::string>{"Conversation"} && task.gap && !task.gap->causeProven,
        "A judged answer became a Skills issue or an established causal claim.");
    Check(!owner.RecordJudgedQualityRetest(id, value, error), "Unchanged automatic retest was admitted.");
    value.dependencies["answer-contract"] = "v2";
    value.judgmentId = "owner-judgment-2";
    value.sourceId = "owner-review-2";
    Check(owner.RecordJudgedQualityRetest(id, value, error), error);
    Check(owner.Snapshot().openTasks.size() == 1, "Failed retest resolved a judged quality gap.");
    SelfAssessmentEngine restarted;
    Check(restarted.Initialize(fixture.root / "assessment.jsonl", error), error);
    auto transport = value;
    transport.source = static_cast<QualityEvidenceSource>(99);
    transport.criterionSatisfied = true;
    Check(!restarted.RecordJudgedQualityRetest(id, transport, error, true), "Unsupported success source resolved the task.");
    value.judgmentId = "owner-judgment-3";
    value.sourceId = "owner-review-3";
    value.targetId = "conversation-turn-3";
    value.targetDigest = revia::audit::ContentDigest("Fresh held-out synthetic answer");
    value.criterionSatisfied = true;
    value.evidence = "The owner checked the fresh held-out reply and found the requested fact present.";
    Check(!restarted.RecordJudgedQualityRetest(id, value, error), "Unchanged dependency retried without explicit request.");
    Check(restarted.RecordJudgedQualityRetest(id, value, error, true), error);
    Check(restarted.Snapshot().openTasks.empty(), "Exact judged acceptance did not resolve the gap.");
    SelfAssessmentEngine closed;
    Check(closed.Initialize(fixture.root / "assessment.jsonl", error) && closed.Snapshot().openTasks.empty(),
        "Resolved judged quality task returned after restart.");
}

void TestSuccessfulRetestNeedsResultFreshAgainstEveryAttempt()
{
    revia::tests::ScopedTestDirectory fixture;
    SelfAssessmentEngine owner;
    std::string error, id;
    Check(owner.Initialize(fixture.root / "assessment.jsonl", error), error);
    auto value = Feedback();
    Check(owner.RecordJudgedQualityFailure(value, id, error), error);
    value.judgmentId = "owner-judgment-result-b-failed";
    value.targetId = "conversation-turn-b";
    value.targetDigest = revia::audit::ContentDigest("Fresh result B still omits the requested fact");
    value.dependencies["answer-contract"] = "v2";
    Check(owner.RecordJudgedQualityRetest(id, value, error), error);
    SelfAssessmentEngine restarted;
    Check(restarted.Initialize(fixture.root / "assessment.jsonl", error), error);
    value.judgmentId = "owner-judgment-result-b-relabeled";
    value.sourceId = "owner-review-result-b-relabeled";
    value.criterionSatisfied = true;
    value.evidence = "The same already-failed result B was relabeled satisfactory.";
    Check(!restarted.RecordJudgedQualityRetest(id, value, error, true),
        "An already-failed retest result resolved the gap under a new owner judgment.");
    Check(restarted.Snapshot().openTasks.size() == 1 && restarted.Snapshot().openTasks.front().gapAttempts.size() == 2,
        "Rejected held-out evidence consumed an attempt or closed the gap.");
    value.judgmentId = "owner-judgment-result-c-passed";
    value.targetId = "conversation-turn-c";
    value.targetDigest = revia::audit::ContentDigest("Fresh held-out result C contains the requested fact");
    value.evidence = "The owner checked the fresh held-out result C against the recorded criterion.";
    Check(restarted.RecordJudgedQualityRetest(id, value, error, true) && restarted.Snapshot().openTasks.empty(), error);
}

void TestCriteriaPrivacyAndUnjudgedDiagnostics()
{
    revia::tests::ScopedTestDirectory fixture;
    SelfAssessmentEngine owner;
    std::string error, id;
    Check(owner.Initialize(fixture.root / "assessment.jsonl", error), error);
    auto value = Feedback();
    for (int missing = 0; missing < 6; ++missing)
    {
        auto invalid = value;
        if (missing == 0)
            invalid.sourceId.clear();
        if (missing == 1)
            invalid.targetDigest.clear();
        if (missing == 2)
            invalid.criterion.clear();
        if (missing == 3)
            invalid.evidence.clear();
        if (missing == 4)
            invalid.dependencies.clear();
        if (missing == 5)
            invalid.origin.sessionId.clear();
        Check(!owner.RecordJudgedQualityFailure(invalid, id, error), "Incomplete quality provenance was accepted.");
    }
    value.privateContextExcluded = true;
    Check(!owner.RecordJudgedQualityFailure(value, id, error), "Excluded private context entered learning evidence.");
    value = Feedback();
    value.evidence = "password=synthetic-secret-credential";
    Check(!owner.RecordJudgedQualityFailure(value, id, error), "Secret evidence entered quality history.");
    revia::runtime::RuntimeEvent event;
    event.component = "Conversation quality";
    event.phase = "Flagged";
    owner.Observe(event);
    event.component = "Conversation";
    event.phase = "Ready";
    owner.Observe(event);
    Check(owner.Assess().openTasks.empty(), "Diagnostic or transport success self-authorized a quality lesson.");
}

void TestFactualCandidateStillNeedsExactReview()
{
    revia::tests::ScopedTestDirectory fixture;
    LearningRecordStore store(fixture.root / "learning");
    LearningCandidate candidate;
    std::string error, id;
    auto value = Feedback();
    value.issue = QualityIssue::MemoryContinuity;
    Check(BuildQualityLearningCandidate(value, candidate, error), error);
    Check(candidate.lesson.kind == LessonKind::Quality && candidate.lesson.sampleSize == 1 &&
              LearningReview::MemoryCategory(candidate.lesson) == "constraint",
        "Scoped quality observation became a personality preference or aggregate claim.");
    Check(store.Initialize(error) && store.Propose(candidate, id, error), error);
    Check(store.Find(id)->disposition == LearningDisposition::PrivateCandidate && !store.PendingAdmission(id),
        "Pending quality candidate became trusted memory.");
    Check(!store.Decide(id, LearningDecision::Accept, "Owner accepts without native checks", error),
        "Judged source bypassed native learning checks.");
    const auto record = *store.Find(id);
    Check(store.Check(id, {record.digest, true, true, true}, error), error);
    Check(store.Decide(id, LearningDecision::Accept, "Exact scoped observation accepted", error), error);
    Check(store.PendingAdmission(id).has_value(), "Checked reviewed quality candidate cannot enter ordinary memory path.");
    value.evidence += " Additional held-out result.";
    value.judgmentId = "owner-judgment-revision";
    value.correctsRecordId = id;
    Check(BuildQualityLearningCandidate(value, candidate, error), error);
    std::string revised;
    Check(store.Propose(candidate, revised, error) && revised != id && !store.PendingAdmission(revised),
        "Changed quality evidence inherited acceptance.");
    LearningRecordStore restarted(fixture.root / "learning");
    Check(restarted.Initialize(error) && restarted.Find(revised)->candidate.correctsRecordId == id,
        "Quality evidence correction lost provenance after restart.");
}

void TestNativeFingerprintsAndMutationAdmission()
{
    revia::tests::ScopedTestDirectory fixture;
    SelfAssessmentEngine owner;
    std::string error, id;
    Check(owner.Initialize(fixture.root / "assessment.jsonl", error), error);
    auto value = Feedback();
    int checks = 0;
    Check(!owner.RecordJudgedQualityFailure(value, id, error, [&] { return ++checks == 1; }) && owner.Snapshot().openTasks.empty(),
        "Judged failure wrote after origin retirement at the persistence boundary.");
    value.dependencies["loaded-profile-sha256"] = revia::audit::ContentDigest("profile-v1");
    value.dependencies["effective-llm-config-sha256"] = revia::audit::ContentDigest("configuration-v1");
    value.dependencies["native-answer-contract"] = "judged-quality-v1";
    value.dependencies["displayed-result"] = value.targetDigest;
    Check(owner.RecordJudgedQualityFailure(value, id, error), error);
    value.judgmentId = "owner-fresh-native-review";
    value.sourceId = "owner-fresh-native-source";
    value.targetDigest = revia::audit::ContentDigest("Fresh result under unchanged configuration");
    value.dependencies["displayed-result"] = value.targetDigest;
    Check(!owner.RecordJudgedQualityRetest(id, value, error), "A new displayed receipt silently counted as changed native configuration.");
    value.dependencies["loaded-profile-sha256"] = revia::audit::ContentDigest("profile-v2");
    Check(owner.RecordJudgedQualityRetest(id, value, error), error);
}

void TestAcceptedBoundsFitExistingOwners()
{
    revia::tests::ScopedTestDirectory fixture;
    LearningRecordStore store(fixture.root / "learning");
    SelfAssessmentEngine owner;
    QualityFeedback value = Feedback();
    value.criterion.assign(1000, 'c');
    value.evidence.assign(2048, 'e');
    value.sourceId.assign(128, 's');
    value.targetId.assign(128, 't');
    value.judgmentId.assign(128, 'j');
    LearningCandidate candidate;
    std::string error, id;
    Check(BuildQualityLearningCandidate(value, candidate, error) && store.Initialize(error) && store.Propose(candidate, id, error) &&
              owner.Initialize(fixture.root / "assessment.jsonl", error) && owner.RecordJudgedQualityFailure(value, id, error),
        "Bounded judged feedback cannot pass the existing learning and gap owners: " + error);
    value.criterion.assign(1024, 'c');
    Check(!ValidateQualityFeedback(value, error), "Feedback admitted a criterion exceeding the downstream evidence bound.");
}
}

void RunQualityFeedbackTests()
{
    TestJudgedFailuresAndRelevantRetests();
    TestSuccessfulRetestNeedsResultFreshAgainstEveryAttempt();
    TestCriteriaPrivacyAndUnjudgedDiagnostics();
    TestFactualCandidateStillNeedsExactReview();
    TestNativeFingerprintsAndMutationAdmission();
    TestAcceptedBoundsFitExistingOwners();
    std::cout << "Judged quality checks passed: exact evidence, bounded retests, exclusion and ordinary reviewed memory admission.\n";
}
