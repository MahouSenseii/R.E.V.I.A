#include "testSupport.h"

#include "Autonomy/activityScheduler.h"
#include "Autonomy/driveState.h"

#include <algorithm>
#include <iostream>

namespace
{
using revia::tests::Check;
using namespace revia::autonomy;
using revia::emotion::EmotionVector;
using revia::emotion::MoodState;
using revia::identity::DevelopmentState;

// A context where she is genuinely free to act: nobody around, budget untouched,
// permissions granted. Any refusal from here is about motivation, not circumstance.
AutonomyCost FreeToAct()
{
    AutonomyCost cost;
    cost.userPresent = false;
    cost.sinceLastUserInteraction = std::chrono::seconds{3600};
    cost.sinceLastActivity = std::chrono::seconds{3600};
    cost.researchAllowed = true;
    cost.observationAllowed = true;
    return cost;
}

DriveState Motivated()
{
    DriveState drives;
    drives[Drive::Curiosity] = 0.9F;
    drives[Drive::Learning] = 0.8F;
    drives[Drive::UnfinishedGoal] = 0.9F;
    drives[Drive::Social] = 0.8F;
    drives[Drive::Exploration] = 0.7F;
    return drives;
}

void TestATimerAloneNeverProducesActivity()
{
    // The single most important property. A timer supplies no evidence, so however long
    // Revia sits idle and however strongly her drives build, idleness alone must never
    // become a reason to act. This is what separates autonomy from a chatterbox.
    const ActivityScheduler scheduler;
    const AutonomyEvidence nothingHappened;
    const DevelopmentState development;

    const ActivityDecision decision = scheduler.Decide(
        Motivated(), nothingHappened, FreeToAct(), EmotionVector{}, MoodState{},
        development);
    Check(decision.type == ActivityType::Nothing,
        "Maxed-out drives with no evidence produced an activity: " +
            ToString(decision.type));
    Check(!decision.refusal.empty(),
        "A refusal carried no reason, so a quiet companion is indistinguishable from a "
        "broken one.");
}

void TestDoingNothingIsTheOrdinaryOutcome()
{
    // Most evaluations should decline. A low bar here is how idle time turns into noise.
    const ActivityScheduler scheduler;
    const DevelopmentState development;
    int acted = 0;
    constexpr int samples = 64;

    for (int index = 0; index < samples; ++index)
    {
        // Mild, ordinary conditions: some evidence some of the time, modest drives, the
        // user around and occasionally busy.
        DriveState drives;
        drives[Drive::Curiosity] = 0.2F + 0.005F * static_cast<float>(index);
        drives[Drive::Boredom] = 0.1F + 0.004F * static_cast<float>(index);

        AutonomyEvidence evidence;
        evidence.desktopChanged = (index % 3 == 0);
        evidence.memoryNeedsTidying = (index % 7 == 0);

        AutonomyCost cost = FreeToAct();
        cost.userPresent = true;
        cost.userIsBusy = (index % 2 == 0);
        cost.activitiesThisHour = index % 4;

        if (scheduler.Decide(drives, evidence, cost, EmotionVector{}, MoodState{},
                development).type != ActivityType::Nothing)
        {
            ++acted;
        }
    }
    Check(acted < samples / 4,
        "Revia acted on " + std::to_string(acted) + " of " + std::to_string(samples) +
            " ordinary idle evaluations, which is restlessness rather than autonomy.");
}

void TestRealEvidenceCanClearTheBar()
{
    // The opposite failure would be just as bad: a scheduler that never acts is not
    // cautious, it is broken.
    const ActivityScheduler scheduler;
    const DevelopmentState development;

    AutonomyEvidence evidence;
    evidence.unfinishedGoal = true;
    evidence.unfinishedGoalImportance = 0.9F;
    evidence.unfinishedGoalId = "goal-42";

    const ActivityDecision decision = scheduler.Decide(
        Motivated(), evidence, FreeToAct(), EmotionVector{}, MoodState{}, development);
    Check(decision.type == ActivityType::ContinueGoal,
        "An important unfinished goal with no competing cost did not get resumed: " +
            ToString(decision.type));
    Check(decision.relatedGoal.has_value() && *decision.relatedGoal == "goal-42",
        "The decision did not carry which goal it meant.");
    Check(!decision.reason.empty(), "An accepted activity carried no reason.");
}

void TestUserPresenceOutranksAutonomousWork()
{
    const ActivityScheduler scheduler;
    const DevelopmentState development;
    AutonomyEvidence evidence;
    evidence.unfinishedGoal = true;
    evidence.unfinishedGoalImportance = 0.9F;

    AutonomyCost midConversation = FreeToAct();
    midConversation.userPresent = true;
    midConversation.conversationActive = true;
    midConversation.sinceLastUserInteraction = std::chrono::seconds{5};

    Check(scheduler.Decide(Motivated(), evidence, midConversation, EmotionVector{},
            MoodState{}, development).type == ActivityType::Nothing,
        "Revia started her own work in the middle of a conversation.");
}

void TestSpeakingIsGatedHardest()
{
    // Speaking is the only activity that interrupts a person, so it carries extra gates
    // beyond the ordinary score.
    const ActivityScheduler scheduler;
    const DevelopmentState development;
    AutonomyEvidence evidence;
    evidence.somethingWorthSaying = true;
    evidence.subjectWorthSaying = "the thing that failed twice";

    AutonomyCost justSpoke = FreeToAct();
    justSpoke.sinceLastUserInteraction = std::chrono::seconds{10};
    const ActivityDecision tooSoon = scheduler.Decide(
        Motivated(), evidence, justSpoke, EmotionVector{}, MoodState{}, development);
    Check(tooSoon.type == ActivityType::Nothing,
        "Revia spoke first moments after the user had been interacting.");
    Check(tooSoon.refusal.find("too recently") != std::string::npos,
        "The refusal did not name the reason: " + tooSoon.refusal);

    AutonomyCost overBudget = FreeToAct();
    overBudget.spokenThisHour = 3;
    Check(scheduler.Decide(Motivated(), evidence, overBudget, EmotionVector{},
            MoodState{}, development).type == ActivityType::Nothing,
        "Revia exceeded her hourly budget for speaking first.");

    // And when the moment genuinely is right, she can.
    const ActivityDecision allowed = scheduler.Decide(
        Motivated(), evidence, FreeToAct(), EmotionVector{}, MoodState{}, development);
    Check(allowed.type == ActivityType::Speak,
        "A real thing worth saying after a long quiet stretch was still refused: " +
            allowed.refusal);
    Check(allowed.subject == "the thing that failed twice",
        "The decision did not carry what she meant to talk about.");
}

void TestMissingPermissionIsRefusedByNameNotSilently()
{
    const ActivityScheduler scheduler;
    const DevelopmentState development;
    AutonomyEvidence evidence;
    evidence.openQuestion = true;
    evidence.openQuestionSubject = "how that library handles retries";

    AutonomyCost noResearch = FreeToAct();
    noResearch.researchAllowed = false;
    const ActivityDecision decision = scheduler.Decide(
        Motivated(), evidence, noResearch, EmotionVector{}, MoodState{}, development);
    Check(decision.type == ActivityType::Nothing,
        "Research happened without permission for it.");

    // "Why did she not look it up?" has to have an answer.
    const std::vector<ActivityDecision> considered = scheduler.ScoreAll(
        Motivated(), evidence, noResearch, EmotionVector{}, MoodState{}, development);
    const bool namedIt = std::any_of(considered.begin(), considered.end(),
        [](const ActivityDecision& candidate)
        {
            return candidate.refusal.find("no permission to research") != std::string::npos;
        });
    Check(namedIt, "A missing permission was silently absent rather than refused by name.");

    // With permission, the same evidence is acted on.
    Check(scheduler.Decide(Motivated(), evidence, FreeToAct(), EmotionVector{},
            MoodState{}, development).type == ActivityType::Research,
        "Granting research permission did not make the open question actionable.");
}

void TestBudgetsAndSpacingHold()
{
    const ActivityScheduler scheduler;
    const DevelopmentState development;
    AutonomyEvidence evidence;
    evidence.unfinishedGoal = true;
    evidence.unfinishedGoalImportance = 1.0F;

    AutonomyCost exhausted = FreeToAct();
    exhausted.activitiesThisHour = scheduler.Limits().maximumActivitiesPerHour;
    Check(scheduler.Decide(Motivated(), evidence, exhausted, EmotionVector{},
            MoodState{}, development).type == ActivityType::Nothing,
        "The hourly activity budget was exceeded.");

    AutonomyCost justActed = FreeToAct();
    justActed.sinceLastActivity = std::chrono::seconds{5};
    Check(scheduler.Decide(Motivated(), evidence, justActed, EmotionVector{},
            MoodState{}, development).type == ActivityType::Nothing,
        "Activities ran back to back with no spacing between them.");
}

void TestDrivesNeedEventsExceptBoredom()
{
    const DriveController controller;
    DriveState drives;

    // Boredom is the one drive time itself creates -- that is what boredom is.
    for (int step = 0; step < 40; ++step)
    {
        drives = controller.Settle(drives, false);
    }
    Check(drives[Drive::Boredom] > 0.3F,
        "Boredom barely accumulated over a long idle stretch (" +
            std::to_string(drives[Drive::Boredom]) +
            "), so the drive exists but can never actually build.");
    Check(drives[Drive::Curiosity] == 0.0F,
        "Curiosity appeared from nothing happening, so a timer manufactured motivation.");
    Check(drives[Drive::UnfinishedGoal] == 0.0F,
        "An unfinished-goal drive appeared with no goal behind it.");

    // Company takes the edge off, which is why being alone and being ignored differ.
    DriveState alone = drives;
    DriveState accompanied = drives;
    for (int step = 0; step < 20; ++step)
    {
        alone = controller.Settle(alone, false);
        accompanied = controller.Settle(accompanied, true);
    }
    Check(accompanied[Drive::Boredom] < alone[Drive::Boredom],
        "Having company did not reduce boredom.");
}

void TestActingOnADriveSpendsIt()
{
    const DriveController controller;
    DriveState drives;
    drives[Drive::Curiosity] = 0.9F;
    const DriveState after = controller.Satisfy(drives, Drive::Curiosity);
    Check(after[Drive::Curiosity] < drives[Drive::Curiosity] * 0.6F,
        "Acting on curiosity did not reduce it, so she would pursue the same thing "
        "forever.");
    Check(after[Drive::Boredom] == drives[Drive::Boredom],
        "Satisfying one drive changed an unrelated one.");
}

void TestActivityLifecycleDistinguishesInterruptionFromDecision()
{
    Check(IsResumable(ActivityStatus::Interrupted),
        "An interrupted activity was not resumable, so the user's attention costs her "
        "the work.");
    Check(IsResumable(ActivityStatus::Paused), "A paused activity was not resumable.");
    Check(!IsResumable(ActivityStatus::Cancelled),
        "A cancelled activity was offered for resumption.");
    Check(IsTerminal(ActivityStatus::Completed) && IsTerminal(ActivityStatus::Failed),
        "A finished activity was not terminal.");
    Check(!IsTerminal(ActivityStatus::Interrupted),
        "An interruption was treated as a final outcome.");

    Activity research;
    research.type = ActivityType::Research;
    research.status = ActivityStatus::Running;
    Check(research.CanBePreemptedBy(ActivityType::Speak),
        "Research could not be preempted by something needing to be said.");

    Activity finished;
    finished.status = ActivityStatus::Completed;
    Check(!finished.CanBePreemptedBy(ActivityType::Speak),
        "A completed activity was preempted.");
}
}

void RunAutonomyTests()
{
    TestATimerAloneNeverProducesActivity();
    TestDoingNothingIsTheOrdinaryOutcome();
    TestRealEvidenceCanClearTheBar();
    TestUserPresenceOutranksAutonomousWork();
    TestSpeakingIsGatedHardest();
    TestMissingPermissionIsRefusedByNameNotSilently();
    TestBudgetsAndSpacingHold();
    TestDrivesNeedEventsExceptBoredom();
    TestActingOnADriveSpendsIt();
    TestActivityLifecycleDistinguishesInterruptionFromDecision();
    std::cout << "Autonomy needs evidence, mostly does nothing, and never lets a timer "
                 "create motivation.\n";
}
