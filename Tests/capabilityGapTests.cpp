#include "testSupport.h"

#include "Learning/selfAssessment.h"

#include <iostream>

namespace
{
using revia::tests::Check;
using namespace revia::learning;

CapabilityGapObservation Observation()
{
    CapabilityGapObservation value;
    value.goal = "synthetic inventory acceptance";
    value.attempts = {"Checked baseline procedure on disposable listing"};
    value.failures = {"No verified procedure satisfied the requested type-count contract"};
    value.reason = CapabilityGapReason::MissingCapability;
    value.cause = "Baseline output lacks separate type counts";
    value.causeProven = true;
    value.missingCapability = "workspace-inventory typed counts";
    value.evidence = "Actual controlled baseline result has total only";
    value.partialEffects = "Read-only listing completed; no data changed";
    value.nextStep = "Review the neutral type-count procedure";
    value.implementationHints = "Existing ListDirectory entry type markers";
    value.retestConditions = "Fresh mixed-entry case verifies all four counts";
    value.owner = "synthetic companion";
    value.priority = 1;
    value.dependencies = {{"skill", "1.0.0"}, {"filesystem", "listing-v1"}};
    return value;
}

void TestRelevantDependenciesAndVerifiedClosure()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto journal = fixture.root / "assessment.jsonl";
    SelfAssessmentEngine owner;
    std::string error, id, duplicate;
    auto observation = Observation();
    Check(owner.Initialize(journal, error) && owner.RecordGap(observation, id, error), error);
    Check(owner.RecordGap(observation, duplicate, error) && id == duplicate, "Repeated gap opened duplicate work.");
    Check(!owner.CanRetest(id, observation.dependencies), "Unchanged dependency retried an already attempted gap.");
    auto unrelated = observation.dependencies;
    unrelated["unrelated"] = "new-version";
    Check(!owner.CanRetest(id, unrelated), "Unrelated dependency caused a retry.");
    auto changed = observation.dependencies;
    changed["skill"] = "1.1.0";
    Check(owner.CanRetest(id, changed), "Relevant changed dependency could not admit a bounded retest.");
    Check(owner.RecordGapAttempt(id, changed, false, false, "Controlled retest did not yet satisfy acceptance", error), error);
    Check(!owner.ResolveTask(id, error), "Generic resolution closed a capability gap without acceptance evidence.");
    SelfAssessmentEngine restarted;
    Check(restarted.Initialize(journal, error), error);
    Check(!restarted.CanRetest(id, changed) && restarted.CanRetest(id, changed, true),
        "Restart lost last-attempt dependency or explicit bounded request admission.");
    Check(!restarted.RecordGapAttempt(id, changed, true, true, {}, error), "Empty proof closed a gap.");
    Check(restarted.RecordGapAttempt(id, changed, true, true, "Actual fresh mixed-entry result satisfies the recorded contract", error),
        error);
    Check(restarted.Snapshot().openTasks.empty(), "Verified acceptance did not resolve its gap.");
    SelfAssessmentEngine resolved;
    Check(resolved.Initialize(journal, error) && resolved.Snapshot().openTasks.empty(), "Resolved gap resurrected after restart.");
}

void TestDistinctFailureCausesAndBoundedAttempts()
{
    revia::tests::ScopedTestDirectory fixture;
    SelfAssessmentEngine owner;
    std::string error, id;
    Check(owner.Initialize(fixture.root / "assessment.jsonl", error), error);
    auto observation = Observation();
    observation.reason = CapabilityGapReason::RevokedAuthority;
    observation.causeProven = false;
    observation.cause = "Revocation is observed; proposed identity cause is unknown";
    observation.missingCapability.clear();
    observation.dependencies = {{"authority", "revision-2"}};
    Check(owner.RecordGap(observation, id, error), error);
    Check(owner.Snapshot().openTasks.front().gap->reason == CapabilityGapReason::RevokedAuthority &&
              !owner.Snapshot().openTasks.front().gap->causeProven,
        "Permission failure was relabelled missing capability or proven cause.");
    Check(owner.RecordGapAttempt(id, observation.dependencies, true, false, "Explicit bounded investigation one", error), error);
    Check(owner.RecordGapAttempt(id, observation.dependencies, true, false, "Explicit bounded investigation two", error), error);
    Check(!owner.CanRetest(id, observation.dependencies, true) &&
              !owner.RecordGapAttempt(id, observation.dependencies, true, false, "Repeated owner request", error),
        "Attempt ceiling allowed a retry storm.");
}
}

void RunCapabilityGapTests()
{
    TestRelevantDependenciesAndVerifiedClosure();
    TestDistinctFailureCausesAndBoundedAttempts();
    std::cout << "Capability-gap checks passed: relevant change, bounded attempts, distinct causes and observed acceptance.\n";
}
