#pragma once

#include "Autonomy/activity.h"
#include "Autonomy/driveState.h"
#include "Emotion/emotionTypes.h"
#include "Emotion/moodState.h"
#include "Identity/developmentState.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace revia::autonomy
{

// Something concrete that happened, which an activity could be a response to.
//
// This is the gate. A timer firing supplies none of these, so a timer alone can never
// produce an activity -- it can only ask whether the evidence already gathered is worth
// acting on. Without that separation, "autonomy" degrades into a random-interval
// chatterbox, which is the single most common way a companion becomes unbearable.
struct AutonomyEvidence
{
    // An approved goal stopped part-way and is still resumable.
    bool unfinishedGoal = false;
    float unfinishedGoalImportance = 0.0F;
    std::string unfinishedGoalId;

    // Something she became curious about during conversation and has not resolved.
    bool openQuestion = false;
    std::string openQuestionSubject;

    // The desktop changed in a way perception already accepted.
    bool desktopChanged = false;

    // Something she was waiting on has finished.
    bool waitEnded = false;

    // The same kind of attempt has failed more than once and is worth reviewing.
    bool repeatedFailure = false;

    // Memory has accumulated enough that consolidating it would help.
    bool memoryNeedsTidying = false;

    // She has something worth saying that came from an actual event, not from silence.
    bool somethingWorthSaying = false;
    std::string subjectWorthSaying;

    [[nodiscard]] bool Any() const;
};

// What it would cost to act right now.
struct AutonomyCost
{
    bool userPresent = false;
    // Actively working. Interrupting is expensive; observing quietly is not.
    bool userIsBusy = false;
    // The user is mid-conversation with her. Nothing autonomous outranks that.
    bool conversationActive = false;

    std::chrono::seconds sinceLastUserInteraction{0};
    std::chrono::seconds sinceLastActivity{0};
    int activitiesThisHour = 0;
    int spokenThisHour = 0;

    // Model and GPU capacity is already committed to something.
    bool resourcesBusy = false;

    // Permissions. Absent authority is not a low score, it is a hard refusal.
    bool researchAllowed = false;
    bool observationAllowed = false;
};

struct SchedulerLimits
{
    // Below this, doing nothing wins. Set high on purpose: most evaluations should
    // decline, and a low bar here is how idle time turns into noise.
    float minimumScore = 0.55F;
    int maximumActivitiesPerHour = 6;
    int maximumSpokenPerHour = 3;
    std::chrono::seconds minimumIntervalBetweenActivities{120};
    // How long the user must have been quiet before speaking first is even considered.
    std::chrono::seconds quietBeforeSpeaking{300};
};

// The decision, and the reasoning behind it either way.
//
// A refusal carries its reason for the same reason an acceptance does: a quiet companion
// that cannot say why she stayed quiet is indistinguishable from a broken one.
struct ActivityDecision
{
    ActivityType type = ActivityType::Nothing;
    float score = 0.0F;
    std::string reason;
    // Populated when type is Nothing.
    std::string refusal;
    std::optional<std::string> relatedGoal;
    std::string subject;
};

// Decides whether there is a reason to do anything at all.
//
// Pure: no clock, no threads, no state. Everything it needs is in its inputs, which is
// what makes "why did she do that?" answerable by replaying the same call.
class ActivityScheduler
{
public:
    explicit ActivityScheduler(SchedulerLimits limits = {});

    [[nodiscard]] ActivityDecision Decide(
        const DriveState& drives,
        const AutonomyEvidence& evidence,
        const AutonomyCost& cost,
        const emotion::EmotionVector& emotion,
        const emotion::MoodState& mood,
        const identity::DevelopmentState& development) const;

    // Every candidate and its score, for the debug panel. Explains not just what she
    // chose but what she considered and rejected.
    [[nodiscard]] std::vector<ActivityDecision> ScoreAll(
        const DriveState& drives,
        const AutonomyEvidence& evidence,
        const AutonomyCost& cost,
        const emotion::EmotionVector& emotion,
        const emotion::MoodState& mood,
        const identity::DevelopmentState& development) const;

    [[nodiscard]] const SchedulerLimits& Limits() const { return limits; }

private:
    SchedulerLimits limits;
};

} // namespace revia::autonomy
