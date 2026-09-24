#include "Learning/learningReview.h"

#include <algorithm>
#include <sstream>

namespace revia::learning
{

std::string ToString(const LessonKind value)
{
    switch (value)
    {
        case LessonKind::Planning: return "planning";
        case LessonKind::Initiative: return "initiative";
    }
    return "planning";
}

std::vector<Lesson> LearningReview::Draw(
    const std::vector<goals::Goal>& recentGoals,
    const initiative::InitiativeCounters& proposalCounters)
{
    std::vector<Lesson> lessons;

    // Only finished goals say anything. One still running has not produced an outcome to
    // learn from, and counting it would make an unfinished run look like a failure.
    std::vector<const goals::Goal*> finished;
    for (const goals::Goal& goal : recentGoals)
    {
        if (goals::IsTerminal(goal.status))
        {
            finished.push_back(&goal);
        }
    }

    if (finished.size() >= MinimumSamples)
    {
        std::size_t succeeded = 0;
        std::size_t verificationFailures = 0;
        std::size_t budgetStops = 0;
        for (const goals::Goal* goal : finished)
        {
            if (goal->status == goals::GoalStatus::Succeeded)
            {
                ++succeeded;
            }
            if (goal->stopReason == goals::StopReason::VerificationFailed)
            {
                ++verificationFailures;
            }
            if (goal->stopReason == goals::StopReason::BudgetActions ||
                goal->stopReason == goals::StopReason::BudgetRetries ||
                goal->stopReason == goals::StopReason::BudgetDuration)
            {
                ++budgetStops;
            }
        }

        // A majority stopping at verification is a statement about the plans, not about
        // the runner: the checks being written do not actually observe what the actions
        // did. That is worth remembering, because it is a habit to correct when planning.
        if (verificationFailures * 2 > finished.size())
        {
            Lesson lesson;
            lesson.id = "lesson-verification";
            lesson.kind = LessonKind::Planning;
            lesson.sampleSize = finished.size();
            lesson.statement =
                "When planning goals, choose a check whose output literally contains the "
                "expected text. Most recent goals stopped because verification did not "
                "observe what the action did.";
            std::ostringstream evidence;
            evidence << verificationFailures << " of " << finished.size()
                << " finished goals stopped at VerificationFailed";
            lesson.evidence = evidence.str();
            lessons.push_back(std::move(lesson));
        }

        if (budgetStops * 2 > finished.size())
        {
            Lesson lesson;
            lesson.id = "lesson-budget";
            lesson.kind = LessonKind::Planning;
            lesson.sampleSize = finished.size();
            lesson.statement =
                "When planning goals, prefer fewer, larger steps. Most recent goals ran "
                "out of budget before finishing.";
            std::ostringstream evidence;
            evidence << budgetStops << " of " << finished.size()
                << " finished goals stopped on a budget ceiling";
            lesson.evidence = evidence.str();
            lessons.push_back(std::move(lesson));
        }

        if (succeeded == finished.size())
        {
            Lesson lesson;
            lesson.id = "lesson-planning-works";
            lesson.kind = LessonKind::Planning;
            lesson.sampleSize = finished.size();
            lesson.statement =
                "The current way of writing goal plans is working; keep the same shape of "
                "act-then-verify step.";
            std::ostringstream evidence;
            evidence << "all " << finished.size() << " recent goals completed and verified";
            lesson.evidence = evidence.str();
            lessons.push_back(std::move(lesson));
        }
    }

    const std::size_t judged =
        proposalCounters.accepted + proposalCounters.dismissed;
    if (judged >= MinimumSamples)
    {
        // Reported either way. A lesson that says "stop doing this" is the one worth
        // having, and a review that only ever confirms itself is not a review.
        Lesson lesson;
        lesson.id = "lesson-initiative";
        lesson.kind = LessonKind::Initiative;
        lesson.sampleSize = judged;
        std::ostringstream evidence;
        evidence << proposalCounters.accepted << " accepted and "
            << proposalCounters.dismissed << " dismissed of " << judged << " judged";
        lesson.evidence = evidence.str();
        lesson.statement = proposalCounters.dismissed > proposalCounters.accepted
            ? "Unprompted observations are mostly unwanted; only speak first when there "
              "is something concrete and unfinished to act on."
            : "Speaking first is landing more often than not; the current threshold for "
              "offering something is about right.";
        lessons.push_back(std::move(lesson));
    }

    return lessons;
}

std::string LearningReview::MemorySummary(const Lesson& lesson)
{
    // Written as a durable preference about how to work, which is what the memory store
    // is for. The evidence travels with it so a stale lesson can be judged later rather
    // than taken on faith.
    return lesson.statement + " (learned from " + lesson.evidence + ")";
}

std::string LearningReview::MemoryCategory(const Lesson& lesson)
{
    // Both kinds are standing preferences about how Revia should work, not facts about
    // the user, so neither is an identity or relationship memory.
    (void)lesson;
    return "preference";
}

} // namespace revia::learning
