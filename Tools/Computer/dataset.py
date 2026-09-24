"""Reading, validating and splitting the recorder's output.

The recorder writes one JSONL file per capture session. This turns those into training
examples, and refuses the ones that must not become training examples.

The refusals are the point of the file. A dataset that quietly admits a row nobody can
say what it demonstrates, or a row whose effect was never confirmed, produces a model
that has learned to do things that might have worked -- and no amount of care later
recovers from that, because by then the lesson is in the weights.
"""

from __future__ import annotations

import hashlib
import json
import os
import random

# Bumped when the meaning of a stored field changes. Rows from a schema this build does
# not implement are refused rather than best-effort read.
SUPPORTED_SCHEMA = 1

# Why a row was refused. Counted rather than logged, so a validation report can say
# "these 40 rows were dropped and here is the reason for each" instead of "some rows
# were dropped".
REASONS = [
    "unsupported_schema",
    "unknown_provenance",
    "not_executed",
    "unverified_effect",
    "weak_verification",
    "no_candidates",
    "no_chosen_candidate",
    "chosen_not_in_mask",
    "redacted_target",
    # The row carries no subgoal descriptor of its own.
    #
    # Written by a build before the descriptor was recorded separately from the chosen
    # candidate. Such a row cannot be used without reconstructing the question from the
    # answer, which is exactly the leak that made the first trained ranker meaningless,
    # so it is refused instead. Old sessions are unusable and that is the correct
    # outcome: they were never usable, it was simply not visible.
    "no_descriptor",
    "malformed",
]

# What a row is evidence *of*.
#
# One dataset, five questions, and they are not interchangeable. A row that proves the
# right field was found says nothing about whether filling it achieved anything; a run
# that correctly did nothing is evidence about abstention and about nothing else. Mixing
# them is how "accuracy" stops meaning anything -- the number goes up when the easy
# family is over-represented, and the family that actually matters is invisible inside it.
FAMILIES = [
    # Was this the right control to act on, out of the ones on offer? The ranking
    # question, and the only family a candidate ranker is trained on.
    "target_selection",
    # Did the action achieve what it was for? Usually unknown for a press, because an
    # accessibility tree carries no evidence that "Zoom in" zoomed in.
    "action_effect",
    # Was the task actually finished? A correctly verified completion may involve no
    # executed action at all -- "it is already in front" is a right answer that touches
    # nothing.
    "completion",
    # Was declining correct? The refusals are the more valuable half of the dataset and
    # they have no chosen candidate by construction, so they can never be ranking rows.
    "abstention",
    # Did this recover from something that had gone wrong? Carries what it replaced.
    "recovery",
]


def family_of(row):
    """Which question this row is evidence about.

    Derived from what the row already says -- what was decided, whether it ran, and what
    the check established. Nothing here is asserted by whatever produced the row, for
    the same reason a postcondition is derived rather than parsed: a decision that
    labelled its own evidence would be marking its own work.
    """
    decision = row.get("decision", "")
    if row.get("corrected_record_id"):
        return "recovery"
    if decision == "propose_completion":
        return "completion"
    # Anything that is not a proposal to act is a decision not to act. Declining is a
    # first-class answer here and the dataset needs it to be one: a set of only
    # successes teaches a policy that it is always right.
    if decision != "propose_action":
        return "abstention"
    if not row.get("executed"):
        return "abstention"
    # An executed action answers two separate questions, and which one it can answer
    # depends on what the check was able to establish.
    #
    # `control_value_is` reads back the exact content that was placed, which is evidence
    # about the effect. `control_state_changed` says only that the control responded --
    # real evidence that the right thing was picked, and no evidence at all about what
    # it did. Treating the second as an effect label is how a policy learns that
    # pressing Delete and pressing Save are equally good.
    checked_by = row.get("checked_by", "text_observed")
    if checked_by == "control_state_changed":
        return "target_selection"
    if checked_by in ("control_value_is", "directory_has_entry", "directory_lacks_entry",
                      "file_contains", "foreground_application_is"):
        return "action_effect"
    return "abstention"


class Example:
    """One decision, as something a ranker can be trained on."""

    def __init__(self, row):
        self.record_id = row["record_id"]
        self.session_id = row.get("session_id", "")
        self.goal_id = row.get("goal_id", "")
        self.subgoal_id = row.get("subgoal_id", "")
        self.intent = row.get("intent", "")
        self.application = row.get("application", "")
        self.provenance = row.get("provenance", "")
        self.candidates = row.get("candidates", [])
        # The question, as the subgoal asked it. Never reconstructed from the chosen
        # candidate: that is the answer, and a feature computed from the answer is a
        # label in disguise.
        self.target_name = row.get("requested_name", "")
        self.target_role = row.get("requested_role", "")
        self.target_container = row.get("requested_container", "")
        # The answer, kept for the record and deliberately not used as a feature.
        self.chosen_name = row.get("target", "")
        self.task_variant = row.get("task_variant", "")
        self.layout_variant = row.get("layout_variant", "")
        self.family = family_of(row)
        # The index of the candidate that was chosen. The label.
        self.label = next(
            (i for i, c in enumerate(self.candidates) if c.get("chosen")), None
        )

    def subgoal(self):
        """The descriptor the subgoal actually carried.

        Read straight off the row. Nothing here looks at `self.label` or at the chosen
        candidate, and that is the whole point: the features must be computable from the
        question alone, or the model is being handed its own answer.
        """
        return {
            "intent": self.intent,
            "target_name": self.target_name,
            "target_role": self.target_role,
            "target_container": self.target_container,
        }


def read_session(path):
    """Every row in one session file, skipping what cannot be parsed.

    A truncated final line is what a crash mid-append leaves behind; it is skipped
    rather than allowed to abort the read, because one unreadable row must not make the
    rows before it unreachable.
    """
    rows = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(row, dict):
                rows.append(row)
    return rows


def read_dataset(root):
    """Every session under `root`, newest last."""
    rows = []
    if not os.path.isdir(root):
        return rows
    for name in sorted(os.listdir(root)):
        if not name.endswith(".jsonl"):
            continue
        rows.extend(read_session(os.path.join(root, name)))
    return rows


def validate(rows):
    """Split rows into what may be trained on and what may not, with reasons.

    Returns (examples, refusals) where refusals is a dict of reason -> count.
    """
    refusals = {reason: 0 for reason in REASONS}
    examples = []
    seen = set()

    for row in rows:
        try:
            if row.get("schema") != SUPPORTED_SCHEMA:
                refusals["unsupported_schema"] += 1
                continue
            if "record_id" not in row:
                refusals["malformed"] += 1
                continue

            # Duplicates. The same row appearing twice is not twice the evidence, and a
            # duplicated example is a silently doubled weight.
            if row["record_id"] in seen:
                refusals["malformed"] += 1
                continue
            seen.add(row["record_id"])

            provenance = row.get("provenance", "unknown")
            if provenance == "unknown":
                refusals["unknown_provenance"] += 1
                continue
            if not row.get("executed"):
                # A proposal is not a demonstration. It may be perfectly sensible and it
                # is still not evidence that it works.
                refusals["not_executed"] += 1
                continue

            outcome = row.get("outcome", "unknown")
            if outcome == "unknown":
                # The effect may have landed. Admitting this as a positive is how a
                # policy learns that an action nobody could confirm is a good action.
                refusals["unverified_effect"] += 1
                continue
            if outcome != "verified":
                refusals["unverified_effect"] += 1
                continue
            if row.get("checked_by", "text_observed") == "text_observed":
                # The substring rule cannot tell "no" from "I could not tell", so it
                # cannot support a label either way.
                refusals["weak_verification"] += 1
                continue

            # The question has to be on the row. A build that did not record the
            # subgoal's own descriptor left only the chosen candidate to read it from,
            # and a feature computed from the chosen candidate is the label wearing a
            # feature's name.
            if not (
                row.get("requested_name")
                or row.get("requested_role")
                or row.get("requested_container")
            ):
                refusals["no_descriptor"] += 1
                continue

            candidates = row.get("candidates", [])
            if not candidates:
                refusals["no_candidates"] += 1
                continue

            chosen = [i for i, c in enumerate(candidates) if c.get("chosen")]
            if not chosen:
                refusals["no_chosen_candidate"] += 1
                continue
            if len(chosen) > 1:
                refusals["malformed"] += 1
                continue
            if candidates[chosen[0]].get("redacted"):
                # The one candidate the row is about was withheld by redaction, so the
                # row cannot say what was chosen without saying what was withheld.
                refusals["redacted_target"] += 1
                continue

            examples.append(Example(row))
        except (KeyError, TypeError, ValueError):
            refusals["malformed"] += 1

    return examples, refusals


def split_by_variant(examples, holdout_fraction=0.3, seed=20260920):
    """Hold out complete task and layout variants.

    Stronger than holding out sessions, and the difference is not academic. Every
    session ran every task, so a session split put "press Save in layout A" in training
    and "press Save in layout A" in evaluation -- the same task, the same arrangement,
    a different run of it. The held-out score was measuring recall of a shape the model
    had already seen, which is exactly the number that looks best and means least.

    A variant is the pair (task, layout). Holding out the pair means the evaluation set
    contains task shapes and window arrangements that were never trained on, which is
    the question anyone actually wants answered.

    Rows with no variant -- ordinary use, where no harness set one -- fall back to the
    session split rather than being silently grouped together. Saying which split was
    used is part of the artifact's lineage.
    """
    variants = sorted(
        {(e.task_variant, e.layout_variant) for e in examples if e.task_variant}
    )
    if not variants:
        return split_by_session(examples, holdout_fraction, seed) + ("session",)

    generator = random.Random(seed)
    generator.shuffle(variants)
    cut = max(1, int(round(len(variants) * holdout_fraction))) if len(variants) > 1 else 0
    held = set(variants[:cut])
    train = [e for e in examples if (e.task_variant, e.layout_variant) not in held]
    test = [e for e in examples if (e.task_variant, e.layout_variant) in held]
    return train, test, sorted("/".join(v) for v in held), "variant"


def by_family(examples):
    """The examples grouped by what they are evidence of."""
    grouped = {family: [] for family in FAMILIES}
    for example in examples:
        grouped.setdefault(example.family, []).append(example)
    return grouped


def split_by_session(examples, holdout_fraction=0.3, seed=20260920):
    """Hold out whole sessions, never adjacent steps.

    Splitting by row would put step 3 of a task in training and step 4 in evaluation.
    The two share a screen, a window and a goal, so the held-out score would be
    measuring memorisation of one task rather than generalisation to another -- and it
    would look excellent.
    """
    sessions = sorted({example.session_id for example in examples})
    generator = random.Random(seed)
    generator.shuffle(sessions)
    cut = max(1, int(round(len(sessions) * holdout_fraction))) if len(sessions) > 1 else 0
    held = set(sessions[:cut])
    train = [e for e in examples if e.session_id not in held]
    test = [e for e in examples if e.session_id in held]
    return train, test, sorted(held)


def lineage(rows):
    """A hash over the exact rows a model was trained on.

    So that "which data produced this artifact?" has an answer that does not depend on
    anyone remembering. Two artifacts with the same lineage saw the same rows.
    """
    digest = hashlib.sha256()
    for record_id in sorted(row.get("record_id", "") for row in rows):
        digest.update(record_id.encode("utf-8"))
        digest.update(b"\n")
    return digest.hexdigest()
