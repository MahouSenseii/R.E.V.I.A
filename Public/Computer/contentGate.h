#pragma once

#include "Computer/computerTypes.h"
#include "Computer/payloadVault.h"
#include "Computer/taskContent.h"
#include "Goals/goalTypes.h"
#include "Windows/targetBinding.h"

#include <cstdint>
#include <atomic>
#include <memory>
#include <string>

namespace revia::computer
{

// What the gate did to one proposed step.
enum class ContentOutcome
{
    // The step enters no text, or the task identified no content. Judged as before.
    NotApplicable,
    // The exact held value was placed. The ordinary success.
    Supplied,
    // A composed value was taken into custody and became the task's value. Only ever
    // happens once per task, and only for a task that asked for composing.
    DraftAdopted,
    // A planner wrote its own text while the runtime was holding the user's. The held
    // value is placed instead, and this is counted -- a silent substitution would hide
    // exactly the defect this gate was built for.
    InventedText,
    // A planner altered a value it had already been given. Refused: the second value is
    // not the task's value, and placing either one is a coin toss.
    ModifiedPayload,
    // The task requires content and the runtime is holding none. The task stops and
    // asks, rather than typing a plausible substitute.
    MissingPayload,
    // The step would enter the content somewhere the user did not ask for. Refused --
    // the right words in the wrong box is a wrong external effect wearing a right
    // intent.
    WrongDestination,
    StaleDraft
};

[[nodiscard]] std::string ToString(ContentOutcome value);

struct ContentDecision
{
    ContentOutcome outcome = ContentOutcome::NotApplicable;
    // Whether the step may proceed. False for every refusal above.
    bool allowed = true;
    // Whether a person has to supply something before this can continue. Distinct from
    // a plain refusal: nothing is wrong, the runtime simply does not have what the task
    // needs, and the answer is a question rather than a failure.
    bool needsInput = false;
    // For the activity feed and the record. Never parsed.
    std::string detail;
    // What the planner tried to write, bounded, so an invented payload is visible in
    // the record rather than only counted. Never the user's own content -- that would
    // put it in a log to prove it had been kept out of one.
    std::string attempted;
};

struct ContentGateStats
{
    // Steps whose text came from the vault rather than from a planner.
    std::uint32_t supplied = 0;
    // Steps where a planner had written something of its own first. The measurement
    // that says whether the grammar constraint is doing its job upstream.
    std::uint32_t inventions = 0;
    std::uint32_t draftsAdopted = 0;
    std::uint32_t modifiedPayloads = 0;
    std::uint32_t wrongDestinations = 0;
    std::uint32_t missingPayloads = 0;
    // Completions refused because the content the task exists to place never landed.
    std::uint32_t prematureCompletions = 0;
};

// The one place a text-entry step gets its text.
//
// Every provider funnels through here -- legacy, routine, learned and the fallback --
// because the defect was never specific to one of them. The routine policy already left
// `value` empty and let the controller redeem it; the legacy path wrote whatever a model
// produced; a learned policy would have done whichever it was built to do. One gate
// downstream of all four is the only arrangement where the guarantee does not depend on
// each provider remembering it.
//
// It holds no authority of its own. It cannot widen a scope, approve an action or skip a
// check: it can only replace a value with the user's, or refuse. Everything it lets
// through is still validated, still policy-checked, still confirmed and still verified.
class ContentGate
{
public:
    // The vault is borrowed and written to, which is the one place this differs from
    // the controller. Adopting a draft means taking custody of a value, and custody is
    // what the vault is.
    using DraftObserver = std::function<actions::windows::DraftSnapshot(
        const actions::ActionRequest&)>;
    explicit ContentGate(PayloadVault& payloadVault,
        DraftObserver observer = actions::windows::ObserveDraft);

    ContentGate(const ContentGate&) = delete;
    ContentGate& operator=(const ContentGate&) = delete;

    // Begins a task with whatever the runtime extracted from the request. Exact content
    // is taken into custody here, before any model is asked anything.
    void BeginTask(TaskContent content);
    void EndTask();

    [[nodiscard]] const TaskContent& Content() const { return task; }
    // The reference a planner may be told about. Carries the kind and the length and
    // never the value.
    [[nodiscard]] const PayloadReference& Held() const { return held; }
    [[nodiscard]] bool Holds() const { return held.Valid(); }

    // Whether the content this task exists to place has actually landed and been
    // verified. Read from the goal's own record, not from anything a provider claimed.
    [[nodiscard]] bool Placed() const { return placed; }

    // Re-read the verified draft and its original context on each iteration. Historical
    // verification alone cannot keep placement current after a person edits the field.
    void ObserveGoal(const goals::Goal& goal);
    [[nodiscard]] bool SubmissionReady() const { return placed && baselineBeforeEntry && !submissionExecuted; }
    [[nodiscard]] bool SubmissionExecuted() const { return submissionExecuted; }

    // Whether a proposed completion may be believed.
    //
    // False while a task that exists to place content has not placed it. This is the
    // "typed something, declared victory" case: a provider that resolved the field, or
    // pressed something, or simply lost track, and answered that the task was done.
    [[nodiscard]] bool CompletionAllowed() const;
    void NoteRefusedCompletion();

    // Supply held text, or bind a committing action to a final live draft check.
    //
    // `context` is only read for the candidate list, which is what turns a control id
    // into something a user's words can be compared against. A step naming a control
    // the observation does not list is left to the executor to refuse; that is its job
    // and not this one's.
    [[nodiscard]] ContentDecision Apply(
        goals::GoalStep& step, const ComputerTaskContext& context);

    [[nodiscard]] const ContentGateStats& Stats() const { return stats; }
    void ResetStats() { stats = ContentGateStats{}; }

private:
    // Whether the step's control is the one the user named. Unknown -- because they
    // named none, or because the observation does not describe that control -- is not
    // a mismatch, and is reported as such rather than refused.
    enum class DestinationVerdict { Unchecked, Matches, Mismatch };
    [[nodiscard]] DestinationVerdict CheckDestination(
        const std::string& control, const ComputerTaskContext& context) const;

    PayloadVault* vault = nullptr;
    TaskContent task;
    PayloadReference held;
    bool placed = false;
    DraftObserver observeDraft;
    actions::ActionRequest placement;
    actions::windows::DraftSnapshot draftBaseline;
    bool baselineBeforeEntry = false;
    bool submissionExecuted = false;
    bool submissionVerified = false;
    ContentGateStats stats;
};

} // namespace revia::computer
