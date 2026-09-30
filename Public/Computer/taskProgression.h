#pragma once

#include "Computer/computerSubgoal.h"
#include "Computer/computerTypes.h"
#include "Computer/taskContent.h"

#include <string>

namespace revia::computer
{

// Runtime evidence determines the next operation. The model resolves targets
// only when deterministic matching is absent or ambiguous.
enum class TaskPhase
{
    // The runtime cannot derive the next operation. Either the task carries no
    // identifiable content, or the screen could not be read. The existing model-driven
    // path answers, exactly as before.
    Undetermined,
    // The application the task is about is not in front. Bringing it forward is not a
    // question anybody needs to be asked.
    AcquireWindow,
    // Content is pending and the destination cannot be identified from the observation.
    // This is the one phase that is *not* derivable, and it is the phase a model is for.
    ResolveDestination,
    // The destination is identified and the content has not landed. The operation is
    // entering the payload, and nothing about that requires a model call.
    PlaceContent,
    // The content is in the field and verified, and the request asked for it to be sent.
    // Reaching this phase at all requires verified placement; there is no path from
    // pending content to here.
    Submit,
    // Everything the request asked for has happened and been verified.
    Complete
};

[[nodiscard]] std::string ToString(TaskPhase value);

// What the runtime derived, and whether it can act on it without asking anything.
struct TaskProgress
{
    TaskPhase phase = TaskPhase::Undetermined;
    // True when `proposed` is filled and the runtime can install it without a model
    // call. False for `Undetermined` and `ResolveDestination`, which are precisely the
    // states where a model has something to contribute.
    bool derivable = false;
    // Derived proposals use the same ValidateSubgoal path and refusal record as model output.
    ComputerSubgoal proposed;
    // For the activity feed and the record. Never parsed.
    std::string detail;
    // Which way the destination failed to resolve, when it did. A description that
    // matches two things is a different question from one that matches none, and they
    // want different help from a model.
    bool destinationAmbiguous = false;
    bool destinationMissing = false;
    // Whether the content this task exists to place has been seen in the field. Carried
    // out so callers do not have to recompute it.
    bool contentPlaced = false;
};

// Everything needed to work out where the task is. All borrowed, nothing owned.
struct TaskProgressInputs
{
    // What the runtime read out of the request.
    const TaskContent* content = nullptr;
    // Whether the content has been seen in a field and verified, from the goal's own
    // record rather than from anything a provider claimed.
    bool contentPlaced = false;
    // Exact draft/control/context checked again since the historical placement.
    bool submissionReady = false;
    // This iteration's single observation.
    const ComputerTaskContext* context = nullptr;
    // Execution ends the submission phase even when verification is uncertain,
    // to prevent duplicate sends. Verification still controls the reported outcome.
    bool submissionDone = false;
    // The reference to the held content, for a payload subgoal.
    PayloadReference payload;
    // The application the task is about. Taken from the goal's approved scope, not from
    // whatever happens to be in front: a window that appears mid-run is not a target.
    std::string application;
    std::string goalId;
};

// Derives phase and proposed subgoal solely from supplied task evidence.
[[nodiscard]] TaskProgress DeriveTaskProgress(const TaskProgressInputs& inputs);

} // namespace revia::computer
