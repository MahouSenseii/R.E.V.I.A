#include "Autonomy/activityScheduler.h"

#include <algorithm>

namespace revia::autonomy
{

namespace
{
    using emotion::Emotion;
    using identity::Trait;

    float Clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }
}

std::string ToString(const ActivityType type)
{
    switch (type)
    {
        case ActivityType::Nothing: return "nothing";
        case ActivityType::Think: return "think";
        case ActivityType::Observe: return "observe";
        case ActivityType::Research: return "research";
        case ActivityType::ContinueGoal: return "continue goal";
        case ActivityType::OrganizeMemory: return "organize memory";
        case ActivityType::Create: return "create";
        case ActivityType::Speak: return "speak";
    }
    return "nothing";
}

std::string ToString(const ActivityStatus status)
{
    switch (status)
    {
        case ActivityStatus::Proposed: return "proposed";
        case ActivityStatus::Running: return "running";
        case ActivityStatus::Waiting: return "waiting";
        case ActivityStatus::Paused: return "paused";
        case ActivityStatus::Interrupted: return "interrupted";
        case ActivityStatus::Completed: return "completed";
        case ActivityStatus::Failed: return "failed";
        case ActivityStatus::Cancelled: return "cancelled";
    }
    return "proposed";
}

bool IsTerminal(const ActivityStatus status)
{
    return status == ActivityStatus::Completed || status == ActivityStatus::Failed ||
        status == ActivityStatus::Cancelled;
}

bool IsResumable(const ActivityStatus status)
{
    // Interrupted and Paused both leave something worth going back to. Waiting does too:
    // whatever it was waiting on may since have arrived.
    return status == ActivityStatus::Interrupted || status == ActivityStatus::Paused ||
        status == ActivityStatus::Waiting;
}

bool Activity::CanBePreemptedBy(const ActivityType urgentType) const
{
    if (IsTerminal(status))
    {
        return false;
    }
    // Anything the user needs outranks anything she chose to do on her own. Speaking is
    // the only activity that competes for the same channel, so it preempts the rest.
    return urgentType == ActivityType::Speak || type != ActivityType::Speak;
}

bool AutonomyEvidence::Any() const
{
    return unfinishedGoal || openQuestion || desktopChanged || waitEnded ||
        repeatedFailure || memoryNeedsTidying || somethingWorthSaying;
}

ActivityScheduler::ActivityScheduler(SchedulerLimits inputLimits)
    : limits(inputLimits)
{
}

std::vector<ActivityDecision> ActivityScheduler::ScoreAll(
    const DriveState& drives,
    const AutonomyEvidence& evidence,
    const AutonomyCost& cost,
    const emotion::EmotionVector& emotion,
    const emotion::MoodState& mood,
    const identity::DevelopmentState& development) const
{
    std::vector<ActivityDecision> candidates;
    const identity::TraitVector traits = development.Current();

    // Costs that apply to everything. Computed once so no candidate can quietly exempt
    // itself from the interruption budget.
    const bool overActivityBudget = cost.activitiesThisHour >= limits.maximumActivitiesPerHour;
    const bool tooSoon = cost.sinceLastActivity < limits.minimumIntervalBetweenActivities;
    const float interruption = cost.conversationActive ? 1.0F
        : cost.userIsBusy ? 0.5F
        : cost.userPresent ? 0.2F : 0.0F;
    const float resourcePressure = cost.resourcesBusy ? 0.45F : 0.0F;
    // Repetition penalty: the more she has already done this hour, the higher the bar.
    const float repetition = 0.12F * static_cast<float>(cost.activitiesThisHour);

    const auto propose = [&](
        const ActivityType type,
        const float motivation,
        const float usefulness,
        const float risk,
        const float interruptionWeight,
        std::string reason)
    {
        ActivityDecision candidate;
        candidate.type = type;
        candidate.reason = std::move(reason);
        // The documented shape: what pulls her toward it, minus what it costs.
        candidate.score = motivation + usefulness
            - risk
            - interruption * interruptionWeight
            - repetition
            - resourcePressure;
        candidates.push_back(std::move(candidate));
    };

    // Nothing is always a candidate and always scores exactly at the bar, so any real
    // activity has to beat it outright rather than win by default.
    ActivityDecision nothing;
    nothing.type = ActivityType::Nothing;
    nothing.score = limits.minimumScore;
    nothing.refusal = evidence.Any()
        ? "There is something she could do, but not enough reason to."
        : "Nothing has happened that would justify doing anything.";
    candidates.push_back(nothing);

    // An active conversation is a hard gate, not a cost to be outweighed. Making it a
    // score penalty meant a strong enough drive could outbid the user's attention, which
    // is exactly backwards: nothing she chose to do on her own outranks someone talking
    // to her.
    if (cost.conversationActive)
    {
        candidates.front().refusal =
            "A conversation is in progress; nothing autonomous outranks that.";
        return candidates;
    }

    // Without evidence, nothing else is even considered. A timer supplies no evidence,
    // so a timer alone can never produce an activity.
    if (!evidence.Any() || overActivityBudget || tooSoon)
    {
        if (overActivityBudget)
        {
            candidates.front().refusal =
                "She has already done enough on her own this hour.";
        }
        else if (tooSoon)
        {
            candidates.front().refusal =
                "She acted too recently to start something else.";
        }
        return candidates;
    }

    if (evidence.unfinishedGoal)
    {
        // Interruption weight is low: continuing her own approved work does not demand
        // anything from the user.
        ActivityDecision candidate;
        propose(ActivityType::ContinueGoal,
            0.55F * drives[Drive::UnfinishedGoal] +
                0.45F * Clamp01(evidence.unfinishedGoalImportance),
            0.35F * Clamp01(evidence.unfinishedGoalImportance),
            0.05F,
            0.3F,
            "an approved goal stopped part-way and is still worth finishing");
        candidates.back().relatedGoal = evidence.unfinishedGoalId;
    }

    if (evidence.repeatedFailure)
    {
        propose(ActivityType::Think,
            0.4F * drives[Drive::Learning] + 0.3F * emotion[Emotion::Frustration],
            0.4F,
            0.0F,
            // Thinking costs nobody anything, so it is barely penalised for the user
            // being present.
            0.05F,
            "the same kind of attempt has failed more than once and is worth reviewing");
    }

    if (evidence.openQuestion)
    {
        if (cost.researchAllowed)
        {
            propose(ActivityType::Research,
                0.6F * drives[Drive::Curiosity] + 0.3F * drives[Drive::Learning],
                0.3F,
                // Research reaches the network. Cautious personalities weigh that more.
                0.15F + 0.2F * traits[Trait::Caution],
                0.35F,
                "something she became curious about is still unresolved");
            candidates.back().subject = evidence.openQuestionSubject;
        }
        else
        {
            // Considered and refused for lack of authority rather than silently absent,
            // so "why did she not look it up?" has an answer.
            ActivityDecision refused;
            refused.type = ActivityType::Nothing;
            refused.score = 0.0F;
            refused.refusal =
                "She has an open question but no permission to research it.";
            candidates.push_back(refused);
        }
    }

    if (evidence.desktopChanged && cost.observationAllowed)
    {
        propose(ActivityType::Observe,
            0.4F * drives[Drive::Exploration],
            0.25F,
            0.0F,
            0.1F,
            "something changed on screen that perception already accepted");
    }

    if (evidence.memoryNeedsTidying)
    {
        propose(ActivityType::OrganizeMemory,
            0.3F * drives[Drive::Learning] + 0.25F * drives[Drive::Boredom],
            0.3F,
            0.0F,
            0.05F,
            "memory has accumulated enough that consolidating it would help");
    }

    if (evidence.somethingWorthSaying)
    {
        // Speaking is the only activity that interrupts a person, so it carries the
        // heaviest weights and the extra gates.
        const bool quietLongEnough =
            cost.sinceLastUserInteraction >= limits.quietBeforeSpeaking;
        const bool underSpeechBudget = cost.spokenThisHour < limits.maximumSpokenPerHour;
        if (quietLongEnough && underSpeechBudget && !cost.conversationActive)
        {
            propose(ActivityType::Speak,
                0.45F * drives[Drive::Social] + 0.3F * traits[Trait::Talkativeness],
                0.35F,
                // Being wrong about whether this is welcome is the expensive mistake, so
                // low sociability and a poor mood both raise the cost of trying.
                0.2F + 0.25F * (1.0F - Clamp01(mood.sociability)),
                1.0F,
                "she has something worth saying that came from an actual event");
            candidates.back().subject = evidence.subjectWorthSaying;
        }
        else
        {
            ActivityDecision refused;
            refused.type = ActivityType::Nothing;
            refused.score = 0.0F;
            refused.refusal = !quietLongEnough
                ? "She has something to say, but the user was interacting too recently."
                : !underSpeechBudget
                    ? "She has something to say, but has already spoken first enough this hour."
                    : "She has something to say, but a conversation is already in progress.";
            candidates.push_back(refused);
        }
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const ActivityDecision& left, const ActivityDecision& right)
        {
            return left.score > right.score;
        });
    return candidates;
}

ActivityDecision ActivityScheduler::Decide(
    const DriveState& drives,
    const AutonomyEvidence& evidence,
    const AutonomyCost& cost,
    const emotion::EmotionVector& emotion,
    const emotion::MoodState& mood,
    const identity::DevelopmentState& development) const
{
    const std::vector<ActivityDecision> candidates =
        ScoreAll(drives, evidence, cost, emotion, mood, development);
    if (candidates.empty())
    {
        ActivityDecision nothing;
        nothing.refusal = "Nothing was even considered.";
        return nothing;
    }
    // Highest score wins, and Nothing sits exactly at the bar, so a real activity has to
    // beat it outright. Ties go to doing nothing.
    const ActivityDecision& best = candidates.front();
    if (best.type != ActivityType::Nothing && best.score > limits.minimumScore)
    {
        return best;
    }

    ActivityDecision nothing;
    nothing.type = ActivityType::Nothing;
    nothing.score = best.score;
    nothing.refusal = best.type == ActivityType::Nothing && !best.refusal.empty()
        ? best.refusal
        : "Nothing cleared the bar for acting.";

    // Prefer a specific refusal over the generic one. A candidate that was ruled out by
    // name -- no permission, spoke too recently, budget spent -- sorts below the
    // baseline because its score is zero, but it is the far more useful answer to "why
    // did she stay quiet?".
    for (const ActivityDecision& candidate : candidates)
    {
        if (candidate.type == ActivityType::Nothing &&
            candidate.score < limits.minimumScore &&
            !candidate.refusal.empty())
        {
            nothing.refusal = candidate.refusal;
            break;
        }
    }
    return nothing;
}

} // namespace revia::autonomy
