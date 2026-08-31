#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace revia::autonomy
{

// What Revia might do when nothing is being asked of her.
//
// Nothing is a first-class member of this list, not an absence. It is also the correct
// answer most of the time: a companion that finds something to do every time it is idle
// is not autonomous, it is restless.
enum class ActivityType
{
    Nothing,
    // Internal only. Reconsider something unresolved, connect two memories, decide an
    // opinion has changed. Produces no output and needs no permission.
    Think,
    // Look at what is already permitted to be looked at.
    Observe,
    // A bounded read-only lookup, behind the existing internet capability.
    Research,
    // Resume an approved goal that stopped part-way.
    ContinueGoal,
    // Housekeeping on her own memory: reinforce, connect, let go.
    OrganizeMemory,
    // Make something -- a diagram, a draft.
    Create,
    // Say something unprompted. The highest-cost activity, because it is the only one
    // that interrupts a person.
    Speak
};

[[nodiscard]] std::string ToString(ActivityType type);

enum class ActivityStatus
{
    Proposed,
    Running,
    // Waiting on something external that has not arrived yet.
    Waiting,
    // Set aside deliberately, expected to resume.
    Paused,
    // Cut off by the user needing attention. Distinct from Paused: an interruption is
    // not a decision, and what was interrupted deserves a chance to continue.
    Interrupted,
    Completed,
    Failed,
    Cancelled
};

[[nodiscard]] std::string ToString(ActivityStatus status);
// Whether an activity has finished for good. A terminal activity is never resumed.
[[nodiscard]] bool IsTerminal(ActivityStatus status);
// Whether it stopped in a way that leaves something worth going back to.
[[nodiscard]] bool IsResumable(ActivityStatus status);

// One thing Revia decided to do, with the reason she decided it.
//
// The reason is not decoration. An autonomous system whose activities cannot be traced
// back to what motivated them is indistinguishable from one acting at random, and the
// difference is the entire point.
struct Activity
{
    std::string id;
    ActivityType type = ActivityType::Nothing;
    ActivityStatus status = ActivityStatus::Proposed;

    // Why she started. Plain language, shown in the debug panel and the activity feed.
    std::string reason;
    // What she is trying to get out of it.
    std::string goal;

    float importance = 0.0F;
    float interest = 0.0F;

    std::chrono::system_clock::time_point startedAt{};
    std::chrono::system_clock::time_point updatedAt{};

    // Set when this activity is continuing an existing approved goal, so resuming goes
    // through the goal runner rather than inventing a parallel execution path.
    std::optional<std::string> relatedGoal;

    // Enough to pick the activity back up. Deliberately a string rather than live
    // state: an interrupted activity must survive the worker that was running it.
    std::string resumeToken;

    [[nodiscard]] bool CanBePreemptedBy(ActivityType urgentType) const;
};

} // namespace revia::autonomy
