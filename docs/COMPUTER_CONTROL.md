# Computer control: who decides the next step

Revia drives the machine through one pipeline: a typed action, checked by
`CapabilityPolicy`, confirmed if it needs confirming, executed by `ActionRuntime`,
written to the audit log, and verified before the run moves on. That pipeline is not
what this document is about, and nothing here changes it.

What this document is about is the question in front of it — *what should she do next?* —
and the fact that it used to have exactly one answer: ask the reasoning model. Many
desktop steps are not reasoning problems. The subgoal says which application, the
observation says whether it is in front, and what to do next follows from those two facts
and nothing else. Sending that to a seven-billion-parameter model is paying reasoning
prices for a lookup, and the person waiting sees it as a pause.

So the decision now has providers, and a mode that chooses between them.

---

## The shape of it

```
user request
  -> Main reads the open-ended request and proposes a bounded subgoal
  -> the runtime validates the subgoal against the task the user authorized
  -> ComputerController picks a provider
  -> one action proposal
  -> GoalRunner / ActionRuntime  (unchanged: policy, confirmation, audit)
  -> observation and verification
  -> continue, stop, or escalate
```

Main is still the only thing that reads a sentence. What it produces is a
`ComputerSubgoal`: a structured intent, a target *description*, an optional reference to
content the runtime holds, and nothing else. It is a claim, not an instruction. The
runtime checks it against the goal's scope, attaches the origin and the budget itself,
and refuses combinations it does not support.

A provider then proposes one action per iteration, from one shared observation. Every
proposal goes through the same validation and the same execution path as a step written
any other way. **A mode chooses who decides. It cannot change what is allowed.**

---

## Modes

| Mode | Who decides | What else happens |
|---|---|---|
| `legacy` | the existing model-driven path, every time | nothing. This is the default, and installing the feature changes no behaviour until someone asks it to |
| `shadow` | the existing path decides *and* executes | the routine policy is asked the same question from the same snapshot; answers are compared and recorded. Nothing it says reaches the machine |
| `assisted` | the routine policy when it is confident, the existing path otherwise | the mode that actually saves the call |
| `learned` | a qualified artifact first, then routine, then the existing path | inactive without a qualified artifact — it falls back to `assisted` rather than deciding with nothing |

Modes switch at a task boundary. Mid-run the providers would change under a goal the
person already approved on the strength of how it was going to be decided.

---

## Using it

```
/controller                                  status: mode, provider, subgoal, counts
/controller mode assisted                    switch who decides
/controller mode legacy                      switch back
/controller record on <app.exe>              open a capture session for one application
/controller record off                       close it
/controller sessions                         what has been recorded
/controller forget <session>                 delete a session's rows
```

`/controller` reports the *selected* mode and the *active* one separately, with the
reason when they differ — a missing artifact, an unqualified one, or a task with no
bounded subgoal are different problems and a person told only "unavailable" cannot tell
which one they can fix.

Configuration lives in `Config/settings.json` under `computerControl`. Every default
preserves existing behaviour:

```json
"computerControl": {
  "providerMode": "legacy",
  "escalationBudget": 3,
  "recordingEnabled": false,
  "datasetDirectory": "ComputerExperience",
  "captureDepth": "structure",
  "learnedArtifactPath": ""
}
```

---

## The routine policy

`RoutineComputerPolicy` is deterministic: no weights, no inference, an answer in
microseconds. It handles launching an approved application, focusing a window, resolving
a target, one interaction with one control, and entering runtime-held content.

What it refuses matters more than what it does. It abstains — by name, with a reason — on:

- **an ambiguous target.** Two controls matching one description is a question, not a
  choice. Answering it by position is answering it by accident.
- **a name that only partly matches.** "Send" does not press "Send later".
- **a truncated listing.** A capped observation that did not contain the target is not
  evidence the target is absent; it asks to look again.
- **a withheld window.** Perception exclusions hold here exactly as they do elsewhere.
- **a target nothing identifies.** Three unlabelled edit boxes in one panel are
  indistinguishable, and picking one is a guess wearing a decision's clothes.
- **anything outside its small set of intents.**

Abstention routes the decision back to Main. That is the correct outcome, not a failure:
a routine policy that guesses when unsure is one that has to be switched off.

An abstention costs a model call. A confident mistake costs an action nobody authorized.
The policy is built around that asymmetry, and the benchmark scores it that way.

---

## Controls with no name

A plain Win32 `EDIT` with nothing beside it publishes no accessible name. `DesktopObserver`
used to drop every such element, so an unlabelled input was invisible to every decision
and the only way to reach one was a raw coordinate — strictly more authority for strictly
less evidence. That was ISSUE-REVIA-0070.

The observer now admits a nameless element when it is **actionable** (advertises a pattern
that does something) and **referable** (has an automation id, a UIA `LabeledBy`
relationship, or a named ancestor). Named controls are admitted first, so the bound on how
many are listed can only ever add candidates and never displace one.

Three kinds of string, kept apart on purpose:

| Field | What it is |
|---|---|
| `name` | what the application published. Frequently empty; that is ordinary. |
| `inferredLabel` | what UI Automation says labels it. An inference. |
| `containerName` | the nearest named ancestor. Where it sits. |

Matching is tiered — published name, then inferred label, then container-and-role — and a
tier is consulted only when the one above found nothing. A guess never outvotes a
statement. More than one match inside a tier is a question, not a choice.

**The safety half.** Admitting nameless controls removed two protections at once: a
nameless password box was previously invisible, and the consequence classifier protects a
*named* one by reading its name. `UIA_IsPasswordPropertyId` is now read for every control,
and a password field never becomes a candidate at any depth.

The fixture keeps three bare fields with no label of any kind, and two named panels each
holding one. The first set is the case that must escalate; the second is the case that
must resolve. Both are demonstrated.

## Who decides what: the runtime owns progression

A 4B Main could produce valid, well-formed subgoals and still choose the wrong
*operation* for the state the task was in. Asked to place a message it proposed
`resolve_target` twice and `interact_with_control` on "Send" once; on a later run it aimed
at a bare edit field instead of the named panel beside it.

None of those is a wording problem. Each was a question the runtime already knew the
answer to and asked anyway — the content was in the vault, the destination was on screen,
and what had to happen next followed from both. No prompt fixes that, because the model is
being asked to rediscover a fact rather than to reason about one.

### The division

> The runtime decides **what kind of operation** comes next, because task state determines
> it and the runtime owns task state.
>
> The model resolves **which thing on screen is meant** — and only when deterministic
> evidence cannot.

That is not a smaller role for the model. It is the role it is good at. "Is the Compose box
the one *labelled* Compose or the one *in* the Compose panel?" is a question about meaning.
"The content is held and the field is identified, what now?" is not.

### The phases

| Phase | Derived? | What happens |
|---|---|---|
| `acquire_window` | **yes** | The application is not in front. Bringing it forward is not a question. |
| `resolve_destination` | **no** | The description matches nothing, or matches two things. **This is the phase a model is for**, and the only one that costs a call. |
| `place_content` | **yes** | Exactly one editable field answers to what the person called it. |
| `submit` | **yes**, when one control answers to the person's own verb | Reachable *only* after verified placement. |
| `complete` | — | Everything asked for has happened. |

Two properties fall out of the shape rather than out of a rule anyone has to remember:

- **No submission before verified placement.** There is no edge from pending content to
  `submit`. `SendBeforePlacement` remains in the validator as a second line, and a derived
  subgoal goes through exactly the same `ValidateSubgoal` a model's does — the runtime has
  no private route to authority.
- **No submission twice.** A press that *executed* ends the phase, verified or not. A send
  that could not be confirmed is precisely the case where repeating it sends twice, so
  "it happened" is the safe reading and "it was confirmed" is the dangerous one.

### What it cost and what it saved

Same three tasks, same fixture, real 4B backend:

| | completed | wall clock | model calls |
|---|---|---|---|
| legacy | 2 of 3 | 31.3s | 7 |
| assisted, before this change | 0 of 3 | 28.6s | 11 |
| **assisted, after** | **2 of 3** | **23.9s** | **4** |

Both content-placement tasks now complete with **zero model calls** — 3.7s and 4.3s,
against 2.6s (failed) and 14.2s for legacy. The remaining four calls are all spent on the
one task with no identifiable content, where there is no progression to derive and the
subgoal planner is still paid for. That is reported rather than hidden: the saving is real
for content tasks and absent for everything else.

---

## Exact content: four locks on one door

When a task involves the person's own words — a message, a subject line, a filename — the
runtime takes custody of them and hands the model a `PayloadReference`: an opaque id, a
coarse kind, and a length. The model decides *where* the content belongs. The runtime
supplies *what* it is, after the action has been authorized.

Two separate reasons, both load-bearing:

- a model that regenerates an exact message sends an approximation of what the person
  wrote, which is a wrong external effect wearing the right intent;
- a payload that travelled through a model is a payload that can end up in a prompt, a
  log, or a training set.

### Why one lock was not enough

A live run showed the arrangement failing completely on the path most runs take. Asked to
place a message, a 4B Main wrote **"The prepared message text here"** into the box and
reported the task complete — and every layer agreed with it. The step ran. The check read
the field back. The typed postcondition compared the field to what had been typed and
found them equal. Three mechanisms, all working exactly as designed, all confirming an
invented sentence, because none of them was holding the real one.

The vault existed. It was simply never filled: `HoldPayload` had no production caller at
all, so on the shipping path there was never anything to substitute.

### What there is now

| Lock | Where | What it stops |
|---|---|---|
| Extraction | `ExtractTaskContent`, before any model call | The runtime not having the words at all. A deterministic parse of the person's own request; no model is asked to extract them, because a model asked to repeat a message can return an approximation of it. |
| Redaction | the task description every provider reads | The words leaking anyway. Holding a message in a vault achieves nothing while the goal's title still contains it, and the title goes into every decision prompt. |
| Grammar | `NextStepSchema` | A constrained decode being *able* to write a sentence where the words go. With content held, `value` admits one fixed token and nothing else — and `type_text` is withdrawn entirely, because typing appends and placing must replace. |
| Gate | `ContentGate`, downstream of every provider | Everything else. Whatever was proposed, what reaches the machine is the held value; a wrong field is refused; a missing payload asks rather than invents; and a completion is refused while the content has not been seen in the field. |

The gate is downstream of the *choice* of provider on purpose. Legacy, routine, learned
and the fallback all arrive at it, so the guarantee does not depend on each provider
remembering it.

### What the planner is left to decide

The destination and the operation. Those are the two questions a model is competent to
answer about someone else's words.

Three operations, kept apart because they are not degrees of the same thing:

- **resolving** finds a control and reports it. Read-only. Finding the Compose box is not
  filling it, and a task whose point is to place content is not finished by having located
  the field.
- **placing** puts the held value in. Verified by reading the field back and comparing it
  to the original.
- **submitting** sends what is in the field somewhere that cannot be retracted. A task to
  place text does not authorize it, and a subgoal proposing it while the content has not
  landed is refused as `SendBeforePlacement` — an *ordering* refusal, distinct from the
  consequence gate's question of whether sending is permitted at all.

### Drafting is a different operation, not a loophole

When the person asks her to *compose* something, the model's text is the point. It is
taken into custody the moment it is chosen and fixed from then on, with its provenance
recorded as `Drafted` rather than `UserSupplied`. A value still living in a planner's head
would be free to come back different on a retry, and the check would then be reading one
sentence while the attempt wrote another.

### What is still true

A reference the vault does not hold redeems to **nothing**, and the decision is refused.
Not an empty string: in a field somebody is about to submit, an empty string is an empty
message actually sent. Payloads are cleared when the task ends, however it ends.

A task whose content the runtime could not identify is judged exactly as it was before any
of this existed. Typing a URL or a search term a planner composed is ordinary work, and a
gate that refused all of it would have broken every task that never had a payload.

---

## Verification, and why a success can read as a failure

A step is verified against a typed postcondition derived by the runtime from the step's
own validated action — never from model output, because a postcondition a model wrote for
itself is a mark it awards itself.

Three outcomes, and they are not two: `Verified`, `Failed`, and `Unknown`. Collapsing
`Unknown` into `Failed` makes a loop retry something that already happened; collapsing it
into `Verified` makes a goal report success it never observed.

Pressing a button leaves **no evidence in a window inspection that the button's effect
occurred**. There is nothing in an accessibility tree that could say "Zoom in zoomed in".
That left every press permanently `Unknown`, which was honest and also meant no press
could ever support a label about anything.

`ControlStateChanged` asks the smaller question the evidence can actually answer: did the
control that was pressed change the state of the window it is in? The runtime takes the
step's own read-only check once **before** the action and once after, and compares what
came back. Both observations are its own, so nothing grades itself, and the cost is one
extra inspection on the steps that use it.

What that establishes is that the target was real, reachable, and did something — a fact
about **target selection**. It says nothing about whether what it did was what was wanted:
pressing Delete and pressing Save both change a window. The dataset keeps those two apart
as separate label families, and a press is admitted as evidence about the first and never
about the second.

It also needs the application to have some observable consequence. The disposable fixture
now reports the control it last activated in a read-only field, which is what real
software does constantly — status bars, activity logs, toasts — and what the fixture had
been quietly doing without.

### ISSUE-REVIA-0069

Under the previous rule a step had to satisfy *both* its typed postcondition and a
substring search for its descriptive `expected` text. For a deletion those two questions
contradict each other: `DirectoryLacksEntry` is proved by a listing the name is absent
from, and the substring rule wants that same name found in that same listing. A deletion
that demonstrably worked came back `Unknown` and stopped the run as an unverified effect.

The typed postcondition is now the contract, under a versioned `Goal::verificationSchema`
so a goal written by an older build keeps the rule it was written with. The descriptive
text is retained for display and recorded as a diagnostic.

One condition on that: the typed postcondition must be **relevant** — the step's own
description has to name the subject the condition is about. A step that announces one
thing and acts on another would otherwise be graded on the thing it acted on and pass,
which is "I created something, so something must be right". Where relevance fails, the
older, stricter reading applies.

---

## Recording

Off. Not off-in-the-configuration: with no capture session open nothing is written, and
opening one does not reach backwards — there is no buffer of earlier activity to flush,
because nothing was kept.

A capture session names **one application** and says what its rows are evidence of. A
session with no named application is a request to record the whole desktop and is not
expressible. Depth is separate: structural metadata (which controls existed, what they
afford, which was chosen) is a different kind of thing from the text in the boxes, and
the second needs its own opt-in.

Credentials are never written at any depth — the check reuses the existing consequence
classifier rather than keeping a second list that could disagree. A redacted candidate
stays in the mask marked as withheld, because a candidate silently missing changes what
the row means.

Deletion removes the rows and reports the artifact lineage that is now downstream of
deleted data. **Deleting a row is not unlearning it.** A model trained on it has already
absorbed whatever it taught, and the honest response is to mark that artifact for
retirement, which is what `/controller forget` says.

---

## Training

`Tools/Computer/` is dependency-free — standard library only, CPU, about a tenth of a
second. Not because a linear model is the end of the road, but because it is the baseline
the road starts from, and a baseline nobody can run is not a baseline.

```bash
# record a controlled domain by actually driving it
ReviaTests.exe --computer-collect RuntimeData/ComputerExperience

# validate, split by task/layout variant, train, evaluate, package
python Tools/Computer/train.py --dataset RuntimeData/ComputerExperience \
                               --out RuntimeData/ComputerArtifacts

# check that training and deployment compute the same thing
python Tools/Computer/parity.py --artifact RuntimeData/ComputerArtifacts/routine-ranker.json \
                                --out expected.json --write-cases cases.json
ReviaTests.exe --computer-parity cases.json expected.json
```

### The funnel

Every number between "a decision happened" and "a row was trained on" is a place where a
dataset can quietly become something other than what it looks like. The trainer prints
all of them, every run:

```text
capture sessions:     1
task/layout variants: 42
raw decisions:        57
admissible labels:    16
  refused (not_executed): 40
  refused (unverified_effect): 1

-- what the admissible rows are evidence of --
  target_selection   12
  action_effect      4
  completion         0
  abstention         0
  recovery           0

trainable as a ranker: 12  (target_selection only)
training examples:    8
held-out examples:    4  (held-out variants: ...)
```

A previous report gave only the ends of this — "21 task variants × 5 sessions" beside
"115 rows" — and the two do not multiply to each other, because sessions and tasks produce
a *variable* number of decisions each. Printing the funnel is the repair: there is no
arithmetic left for a reader to have to reconstruct.

### Five families, not one pile

A row is evidence about one question, and the questions are not interchangeable:

| Family | The question | Note |
|---|---|---|
| `target_selection` | Was this the right control, out of the ones on offer? | The only family a candidate ranker is trained on. |
| `action_effect` | Did the action achieve what it was for? | Usually unknown for a press. |
| `completion` | Was the task actually finished? | May involve no executed action at all — "it is already in front" is a right answer that touches nothing. |
| `abstention` | Was declining correct? | No chosen candidate by construction, so never a ranking row. The more valuable half of the dataset. |
| `recovery` | Did this repair something that had gone wrong? | Carries what it replaced. |

Mixing them is how "accuracy" stops meaning anything: the number rises when the easy
family is over-represented, and the family that matters disappears inside it.

### The leak that explains the first ranker

The first trained artifact put **+5.59 on `position_normalised` and 0.00 on `name_exact`**.
It had learned where things sat. Two causes, and both were in the pipeline rather than in
the model:

1. **The question was reconstructed from the answer.** The recorder stored the chosen
   control and no descriptor, so the dataset builder rebuilt `target_name` and
   `target_role` from the chosen candidate. Every name and role feature was therefore 1
   for the correct answer *by construction*. The record now carries `requestedName`,
   `requestedRole` and `requestedContainer` — the question — strictly apart from `target`
   and `chosen`, which are the answer. A row written before that split is refused as
   `no_descriptor` rather than used.

2. **The design matrix could not express the rows.** Every admissible row was an entry
   into an unnamed field, where `name_exact` is 0 for every candidate. The only column
   that varied was position, so position is what was learned. Feature version 2 adds
   `container_exact`, `container_contains`, `label_exact`, `label_contains`,
   `candidate_nameless` and `descriptor_named_container` — the evidence a person actually
   uses to tell one unlabelled box from the next.

Retrained with both repaired, on the same collection protocol: **`name_exact +7.08`,
`position_normalised −0.60`**. Names, and a negative weight on position.

### Splits hold out variants, not sessions

Splitting by session was not enough: every session ran every task, so the same task shape
appeared on both sides and the held-out score measured recall of something already seen.
The recorder now stamps a task and layout variant on every row, the fixture builds the
same controls in two arrangements, and the split holds out **complete (task, layout)
pairs**. Rows with no variant fall back to a session split, and the trainer says loudly
when that happens.

Validation still refuses rows that must not become labels, and still says which: unknown
provenance, never executed, unverified effect, verification by substring only, no
candidate mask, a redacted target, no descriptor.

An artifact carries its weights, feature contract, abstention threshold, qualified scope,
dataset lineage, held-out numbers, and a behaviour hash. The runtime recomputes that hash
at load and refuses an artifact whose weights or scope changed after it was evaluated —
its held-out numbers would be numbers about a different function. It also refuses an
unsupported feature version (weights are positional; applied to the wrong columns they
are a different function, not a worse one), a missing scope, no held-out evaluation, and
any artifact that made a confident mistake on held-out data.

Outside its qualified scope the artifact declines. A number measured in one application
is not a number about another.

---

## Qualification: what has to be true before "it works"

The fixture is not evidence about anything but the fixture, and a matrix of real
applications is not evidence about applications it did not run in. So the claims here are
made per application and per task family, and the gates below are enforced in code rather
than kept as a note.

### Five states, and nothing may skip one

| State | Means |
|---|---|
| **implemented** | the code exists and compiles. Says nothing else. |
| **fixture-tested** | covered by tests that fail without the repair. No model, no desktop. |
| **real-model demonstrated** | driven through the real entry point with a real backend, nothing scripted. |
| **resource-measured** | numbers off the hardware, not off a state field. |
| **qualified** | works in a named application, for a named task family, on evidence that meets the floors below. |

### The floors, and why each one exists

Enforced by `QualificationFloor` in `Public/Computer/learnedPolicy.h`, checked when an
artifact is loaded. Each is a way a policy can look good without being good.

| Axis | Floor | The failure it catches |
|---|---|---|
| held-out examples | 50 | Zero mistakes out of four is not a rate. |
| applications | 2 | One application's controls are one vendor's conventions. |
| task families | 3 | A policy evaluated only on "press a named button" says nothing about placing content in an unnamed field. |
| layout variants | 2 | The first trained artifact put **+5.59 on position**. A held-out set that never moves anything cannot catch that. |
| sessions | 3 | Adjacent steps in one session share a screen; splitting inside one measures memorisation. |
| coverage | 0.40 | Below this the artifact abstains on so much that the calls it was meant to save are still being made. |
| confident mistakes | **0** | An abstention costs a model call. A confident mistake costs a click nobody authorized. |

These are floors, not targets. Meeting them makes an artifact *eligible* to be compared
against the deterministic policy. It does not make it better than one, and the comparison
is still the thing that decides.

### What that means for the artifact that exists

Refused, at load:

```text
REFUSED at load: not_qualified
  The artifact is not qualified: held-out examples 4, and a target-selection policy
  needs at least 50. Zero mistakes on a narrow held-out set is not evidence that it
  generalises.
```

It learns the right thing now — `name_exact +7.08`, `position_normalised −0.60` — and it
is still not allowed to decide anything. The deterministic policy remains primary; learned
mode stays experimental until a genuinely diverse held-out set exists.

### What is qualified today

Nothing. The matrix is evidence about three applications and eight task shapes, which is
not enough for any of the floors above, and it is reported as what it is.

---

## What has actually been measured

### With a real model (`--computer-live 8099`)

A llama.cpp server running the configured Main model (Qwen3.5-4B-Q4_K_M), driven through
`/operate` — the same entry point a person uses. Nothing scripted.

#### Legacy against assisted, same three tasks

| Task | Mode | Completed | Wall clock | Model calls | of which subgoal | Exact content placed |
|---|---|---|---|---|---|---|
| press Zoom in | legacy | **yes** | 15.2s | 3 | 0 | – |
| press Zoom in | assisted | no | 15.1s | 4 | 3 | – |
| place content in Compose | legacy | no | 2.3s | 1 | 0 | 0 |
| place content in Compose | assisted | no | 4.5s | 3 | 2 | 0 |
| place content in Subject | legacy | no | 16.7s | 2 | 0 | 1 |
| place content in Subject | assisted | no | 9.1s | 4 | 3 | 1 |
| **totals** | legacy | **1 of 3** | **34.2s** | **6** | 0 | 1 |
| **totals** | assisted | **0 of 3** | **28.6s** | **11** | 8 | 1 |

**Net: assisted spent five more model calls and finished 5.6 seconds sooner.**

That is the measurement the previous delivery did not take. It reported "3 of 4 step
decisions taken without a model" and let that stand as the saving; on these tasks the
subgoal planning more than eats it. A feature that spends two calls to save three has
saved one, and the only way to tell that from spending four to save three is to count
both sides. On short tasks — and all of these are short — assisted mode costs more calls
than it avoids.

The planner wrote its own text **zero** times across all six runs. The grammar constraint
is doing its work upstream, so the content gate never had to repair an invention.

#### What the completions say

One of six. Worth reading carefully, because none of it is the arrangement failing:

- **Wrong field, refused.** For the Compose task the model repeatedly aimed at
  `document_field` — a bare edit — rather than the field inside the Compose panel. The
  destination check refused it: *"The content was to go in compose and this step would put
  it somewhere else."* Before this round, that task "succeeded" by typing invented text
  into the wrong box.
- **`wait_for_state` at the end.** Having pressed Zoom in successfully twice, the model's
  next subgoal was "the Zoom in button has already been clicked successfully" as
  `wait_for_state`. The run ended `undecided` rather than complete. The legacy path, asked
  the whole-task question, answered "complete" correctly.
- **A confused step.** One Subject run proposed `invoke_control` on the field with an
  expected string about content. Nothing entered text, so the content gate correctly left
  it alone, and verification correctly could not establish it.

All three are intent-selection by a 4B model. The path underneath them is sound: when the
model picks `interact_with_control`, four of five step decisions are taken with no model
at all.

#### Shadow mode

| | |
|---|---|
| comparisons | 5 |
| agreed with the live path | 1 of 5 |
| decisions the shadow executed | **0** |

Both providers were asked from one observation. The shadow's answer never reached the
machine, and the live target was not invalidated by the comparison — which is what a
shadow taking its own look would have done, by bumping the observation generation and
making the live provider's target stale.

### The generalization matrix (`--generalization-matrix [port]`)

Three applications, thirteen cases, every task reversible and local. Applications are
started and terminated by the run; nothing is saved, sent outside the fixture, purchased
or deleted.

| Application | Family | Requested | Actual | Calls | Derived | Verification | Outcome |
|---|---|---|---|---|---|---|---|
| notepad.exe | exact text, unnamed field | Text Editor | Text editor | **0** | 1 | verified (control_value_is) | **completed** |
| notepad.exe | destination not there | Recipient | – | 2 | 0 | nothing executed | abstained |
| notepad.exe | no destination named | (none) | – | 2 | 0 | nothing executed | abstained |
| charmap.exe | exact text, named field | Characters to copy | 104 | 3 | 0 | **failed** (control_value_is) | verification_failed |
| charmap.exe | legacy, same task | Characters to copy | 104 | 1 | 0 | **failed** (control_value_is) | verification_failed |
| fixture | unnamed field in named panel | Compose | 1017 | **0** | 1 | verified | completed |
| fixture | second text field | Subject | 1018 | **0** | 1 | verified | completed |
| fixture | ambiguous: three bare fields | Document | – | 3 | 0 | nothing executed | abstained |
| fixture | placement only, no send | Compose | 1017 | **0** | 1 | verified | completed |
| fixture | placement then submission | Compose then Send | 1006 | **0** | 2 | unknown | sent, unverified |
| fixture | cancelled before it ran | – | – | 0 | 0 | nothing executed | cancelled |
| fixture (layout b) | unnamed field, reordered | Compose | 1017 | **0** | 1 | verified | completed |
| fixture (layout b) | second field, reordered | Subject | 1018 | **0** | 1 | verified | completed |

```text
cases run:            13
completed:             6
abstained:             4   (not a failure: declining an unidentifiable target is correct)
confident wrong acts:  0   (the only number here that is ever a defect)
model calls:          11
operations derived:    8   (settled from task state, no call spent)
submission ordering:  content verified, then sent
cancellation:         nothing executed
```

**Notepad generalised, and a gap had to be closed first.** Its editing surface is a
Document control, not an Edit, and the executor only read a value pattern from Edits — so
text could be placed and never read back. The exact-content check returned Unknown on the
very first real application it met while working perfectly on the fixture. Widened to
Document; the password guard is unchanged and still runs first.

**Character Map is the most useful row in the table.** The field was found, text was
entered, and the check came back **failed** — charmap repopulates that box from the
character grid, so what was typed does not stay. The run stopped and reported a failure
instead of claiming success. A system that trusted "the action returned without error"
would have reported this as done.

**Layout b changes nothing**, which is the point: the same names in different places and a
different enumeration order, same results, zero calls.

**What the matrix does not establish.** Three applications and eight task shapes meet none
of the qualification floors above. This is evidence about these applications and these
tasks.

### Cancellation, on an owned server (`--cancellation-live`)

ISSUE-REVIA-0066, settled by measurement rather than argument.

```text
idle, before anything          slots=2  processing=0  cached=[0,0]
during generation              slots=2  processing=1  cached=[0,13]
client released after          77ms
immediately after cancelling   slots=2  processing=1  cached=[0,40]
1.5s after cancelling          slots=2  processing=0  cached=[0,41]
next request took              2578ms  (answered)
```

The three conditions the finding could not distinguish are now distinguished. There is a
brief tail — the server was still producing at the instant the client let go, which is
generation in flight and not a stuck slot. By 1.5 seconds the slot is free. What remains
held is the prompt cache, which is the benign case and is what makes the next turn fast.

**A cancelled decision does not hold inference capacity.**

### Synthetic comparison (`ReviaTests.exe`, always printed)

Labelled SYNTHETIC in the output. The baseline is a substring matcher standing in for a
model-driven path; it reaches no backend and measures the *shape* of the difference.

| | correct | wrong | declined | model calls | µs/decision |
|---|---|---|---|---|---|
| routine | 160 | **0** | 80 | **0** | ~8 |
| substring stand-in | 140 | 40 | 60 | 240 | ~2 |

### The learned ranker (`--learned-compare <artifact>`)

Retrained after the label leak was closed and the context features added.

**What it learned this time:** `name_exact +7.08`, `role_exact +0.61`,
`affords_intent +0.61`, `candidate_nameless −0.61`, **`position_normalised −0.60`**. The
previous artifact had +5.59 on position and 0.00 on names. Held out on complete
task/layout variants: 4 examples, coverage 1.00, accuracy 1.00, zero confident mistakes.

**And it still does not earn its place.** Against the deterministic policy on the standard
cases, built in the artifact's own qualified application:

| | correct | wrong | coverage |
|---|---|---|---|
| routine | 160 | **0** | 0.67 |
| learned | 80 | **40** | 0.50 |

**Verdict: retain the deterministic policy.** The abstention threshold was calibrated on
four held-out examples, which is far too few to find a safe one, and the ranker acts
confidently wrong on cases the routine policy declines.

One note on the previous report's "0 of 240": that number measured the *scope gate*, not
the ranker. The comparison built its cases in an application the artifact was never
evaluated in, so the runtime refused every one of them — correctly. The cases are now
built in the artifact's own qualified application, and the comparison says something.

### Residency, on the cards (`--residency-live`)

Real llama.cpp Expert server (Qwen3-VL-8B-Q4_K_M), driven through the real
`ModelLifetimeCoordinator` and the real process owner, measured with `nvidia-smi`. Per
device and in aggregate, because the aggregate alone hides which card took the model.

**Launch configuration**, printed because without it the split below is a number with no
explanation: context 4096, one parallel slot, vision off, **no device split specified** —
llama.cpp places layers itself, which is what the application does. The split is therefore
whatever the runtime chose on this machine, not a property of the design.

| Device | before | loaded | unloaded | held | residual |
|---|---|---|---|---|---|
| gpu0 (RTX 5070) | 5,430 | 8,651 | 5,426 | **+3,221** | −4 |
| gpu1 (RTX 2070 SUPER) | 1,577 | 3,980 | 1,577 | **+2,403** | +0 |
| aggregate | 7,007 | 12,631 | 7,003 | **+5,624** | **−4 MiB** |

The residual is **−4 MiB, raw and unclamped**: the machine ended four megabytes *below*
where it started, because other processes moved while this ran. The previous report said
"0 MiB residual", which turned measurement noise into a claim of exactness. The honest
statement is that the end state **approximately returned to baseline**, and the raw figure
is printed so a reader can judge the approximation themselves.

**Cold and warm cycles**, three of each:

| | cold start | warm acquire |
|---|---|---|
| cycle 1 | 3,700 ms | 0 µs |
| cycle 2 | 3,647 ms | 0 µs |
| cycle 3 | 3,672 ms | 0 µs |

A warm acquire does not stall a turn, which is the responsiveness question this file can
answer. **What it cannot:** end-to-end speech latency while a tier is actually loading.
That needs the speech worker and a clock on the audio, and is not measured here.

Also verified: an idle sweep does not evict a held lease; five repeated sweeps caused no
reload thrash; a cancelled acquisition starts no load; `Stop()` on a process this run did
not start does nothing.

**An `Unloaded` state is a label. These are the numbers.**

### And the finding that matters most about all of it

```text
fast      on_demand=no  warm_at_startup=yes  idle_grace=300s  min_residency=60s
expert    on_demand=no  warm_at_startup=yes  idle_grace=300s  min_residency=60s
```

**Expert is not on-demand in the shipped settings.** Every figure above is a measurement
of a mechanism that works and that this machine is not currently using. The switch exists,
it is documented, it defaults to off deliberately — trading first-use latency for memory
is a choice about a particular machine — and nobody has turned it on. A report that
described the savings without saying this would have been describing savings nobody is
taking.

**What has not been established:** that any of this generalises beyond the controlled
fixture.
