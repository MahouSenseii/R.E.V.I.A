#pragma once

#include "Computer/computerSubgoal.h"
#include "Computer/computerTypes.h"
#include "Computer/taskContent.h"

#include <string>

namespace revia::computer
{

// Where a content task has got to, decided by the runtime from evidence it already has.
//
// The defect this exists to close: Main could produce valid, well-formed subgoals and
// still choose the wrong *operation* for the state the task was in. Asked to place a
// message it proposed `resolve_target` twice and `interact_with_control` on "Send" once,
// and on a later run it aimed at a bare edit field instead of the named panel. Every one
// of those was a decision the runtime did not need to ask about: the content was in the
// vault, the destination was on screen, and what had to happen next followed from both.
//
// The division of labour this introduces:
//
//   the runtime decides *what kind of operation* comes next, because task state
//   determines it and the runtime owns task state;
//
//   the model resolves *which thing on screen is meant*, but only when deterministic
//   evidence cannot -- when a description matches nothing, or matches two things.
//
// That is not a smaller role for the model. It is the role it is actually good at. A
// model asked "is the Compose box the one labelled Compose or the one in the Compose
// panel?" is being asked a question about meaning. A model asked "the content is held,
// the field is identified, what now?" is being asked to rediscover a fact the runtime
// wrote down, and it will sometimes get it wrong -- which is not a prompt problem, it is
// a design problem, and no wording fixes it.
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
    // Proposed, and therefore still validated. This is deliberately not a validated
    // subgoal: it goes through the same `ValidateSubgoal` a model's proposal does, so
    // there is exactly one path by which a subgoal acquires authority and the runtime
    // does not get a private one. A derived subgoal that fails validation is refused for
    // the same reasons and with the same record.
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
    // Whether a submission has already been *executed* for this task.
    //
    // Executed, deliberately, and not verified. A send that ran and could not be
    // confirmed is exactly the case where repeating it sends twice -- so for deciding
    // whether the submission phase is over, "it happened" is the safe reading and
    // "it was confirmed" is the dangerous one. Verification still decides what the run
    // *reports*; it does not decide whether to do it again.
    bool submissionDone = false;
    // The reference to the held content, for a payload subgoal.
    PayloadReference payload;
    // The application the task is about. Taken from the goal's approved scope, not from
    // whatever happens to be in front: a window that appears mid-run is not a target.
    std::string application;
    std::string goalId;
};

// Work out the phase, and derive the subgoal when the phase determines one.
//
// Pure with respect to everything but its inputs, so the whole progression is testable
// without a desktop, a model or a session -- which matters, because the states this has
// to get right are exactly the ones that are expensive to reach by hand.
[[nodiscard]] TaskProgress DeriveTaskProgress(const TaskProgressInputs& inputs);

} // namespace revia::computer
