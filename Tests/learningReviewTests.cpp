#include "testSupport.h"

#include "Learning/learningRecordStore.h"

#include <fstream>
#include <iostream>

namespace
{
using revia::tests::Check;
using namespace revia::learning;

LearningCandidate Candidate(const std::size_t dismissed)
{
    revia::initiative::InitiativeCounters counters;
    counters.accepted = 1;
    counters.dismissed = dismissed;
    LearningCandidate value;
    value.lesson = LearningReview::Draw({}, counters).front();
    value.evidence.sources = {"synthetic:initiative-counter"};
    value.evidence.conditions = {"Disposable counters; observed judged outcomes only"};
    value.evidence.supporting = {value.lesson.evidence};
    value.evidence.checkedAt = "2026-10-03T00:00:00Z";
    value.origin.companionId = "synthetic-A";
    value.origin.sessionId = "session-A";
    value.origin.generation = 1;
    return value;
}

void TestImmutableEvidenceRevisionAndRejectedHistory()
{
    revia::tests::ScopedTestDirectory fixture;
    LearningRecordStore store(fixture.root / "learning");
    std::string error, first, changed, duplicate;
    Check(store.Initialize(error), error);
    Check(store.Propose(Candidate(7), first, error) && store.Propose(Candidate(10), changed, error), error);
    Check(first != changed, "Changed evidence inherited a prior candidate's review identity.");
    Check(store.Propose(Candidate(7), duplicate, error) && duplicate == first, "Exact candidate was not deduplicated.");
    Check(!store.Decide(first, static_cast<LearningDecision>(999), "Synthetic unsupported decision", error) &&
              store.Find(first)->decision == LearningDecision::Pending && store.Find(first)->reviews.empty(),
        "An unsupported decision corrupted durable review history.");
    Check(store.Decide(first, LearningDecision::Reject, "Counts do not establish the proposed cause.", error), error);
    Check(!store.PendingAdmission(first), "Rejected candidate became eligible trusted content.");
    Check(!store.RecordAdmission(first, true, "forged-memory-receipt", error), "Rejected candidate accepted a memory receipt.");
    LearningRecordStore restarted(fixture.root / "learning");
    Check(restarted.Initialize(error), error);
    Check(restarted.Find(first)->decision == LearningDecision::Reject && restarted.History().size() == 2,
        "Rejected history or evidence revision did not survive restart.");
}

void TestChecksCannotBeOverriddenAndMemorySaveIsRetryable()
{
    revia::tests::ScopedTestDirectory fixture;
    bool admitted = true;
    LearningRecordStore store(fixture.root / "learning", [&] { return admitted; });
    std::string error, id;
    Check(store.Initialize(error) && store.Propose(Candidate(7), id, error), error);
    Check(!store.Decide(id, LearningDecision::Accept, "Parent says yes", error), "Parent acceptance overrode absent checks.");
    auto record = *store.Find(id);
    LearningChecks checks{record.digest, true, false, true};
    Check(store.Check(id, checks, error), error);
    Check(!store.Decide(id, LearningDecision::Accept, "Parent says yes", error), "Parent acceptance overrode failed privacy.");
    checks.privacySafe = true;
    Check(store.Check(id, checks, error) && store.Decide(id, LearningDecision::Accept, "Observed bounded evidence accepted", error), error);
    Check(store.PendingAdmission(id).has_value(), "Eligible accepted candidate could not be submitted through existing memory owner.");
    Check(store.RecordAdmission(id, false, {}, error), error);
    Check(store.Find(id)->disposition == LearningDisposition::MemorySaveFailed && store.PendingAdmission(id),
        "Failed semantic save was counted trusted or could not be retried.");
    admitted = false;
    Check(!store.RecordAdmission(id, true, "memory-1", error), "Stale session committed an admission receipt.");
    admitted = true;
    Check(!store.RecordAdmission(id, true, {}, error), "Unknown memory receipt was counted trusted.");
    Check(store.RecordAdmission(id, true, "memory-1", error), error);
    Check(store.Find(id)->disposition == LearningDisposition::TrustedMemory && !store.PendingAdmission(id),
        "Confirmed semantic save was not recorded exactly once.");
}

void TestCorrectionAndStrictPersistence()
{
    revia::tests::ScopedTestDirectory fixture;
    LearningRecordStore store(fixture.root / "learning");
    std::string error, first, corrected;
    Check(store.Initialize(error) && store.Propose(Candidate(7), first, error), error);
    auto correction = Candidate(10);
    correction.correctsRecordId = first;
    correction.evidence.contradicting = {"synthetic:the earlier causal claim was not measured"};
    Check(store.Propose(correction, corrected, error), error);
    Check(store.Find(corrected)->candidate.correctsRecordId == first && store.Find(first)->candidate.evidence.supporting.size() == 1,
        "Correction destroyed the original evidence or lost its explicit link.");
    std::ofstream(fixture.root / "learning" / "records.json", std::ios::app) << "\n{}";
    LearningRecordStore invalid(fixture.root / "learning");
    Check(!invalid.Initialize(error), "Second JSON document admitted private learning history.");
}
}

void RunLearningReviewTests()
{
    TestImmutableEvidenceRevisionAndRejectedHistory();
    TestChecksCannotBeOverriddenAndMemorySaveIsRetryable();
    TestCorrectionAndStrictPersistence();
    std::cout << "Learning review checks passed: private revisions, rejection, objective admission and retryable semantic receipts.\n";
}
