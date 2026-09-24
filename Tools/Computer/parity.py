"""Check that training and deployment compute the same thing.

    python Tools/Computer/parity.py --artifact <artifact.json> \
                                    --cases <cases.json> \
                                    --out <expected.json>

An artifact is a set of weights over a feature vector. Training computes that vector in
Python; the runtime computes it again in C++. Nothing enforces that the two agree except
a check that computes both and compares -- and a mismatch is the quietest failure in the
whole pipeline, because the model still runs, still returns a candidate, and is simply
answering a different question than the one it was evaluated on.

This is the Python half. It writes what Python says for a set of cases; the C++ half
(`ReviaTests.exe --computer-parity`) reads the same cases, computes its own answers, and
refuses to agree unless they match.

Cases are deliberately supplied rather than generated here, so both halves read the same
file and neither can quietly disagree about what the inputs were.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import features as feature_module

# Feature values are exact small numbers -- ones, zeros, and a normalised position that
# is a ratio of small integers. Scores are dot products of a dozen such terms. Anything
# beyond this is not floating-point noise, it is a different computation.
FEATURE_TOLERANCE = 1e-12
SCORE_TOLERANCE = 1e-9


def softmax(scores):
    highest = max(scores)
    exponentials = [math.exp(score - highest) for score in scores]
    total = sum(exponentials)
    return [value / total for value in exponentials]


def evaluate_case(artifact, case):
    """What Python says about one observation."""
    subgoal = case["subgoal"]
    candidates = case["candidates"]
    rows = feature_module.design_matrix(subgoal, candidates)
    weights = artifact["weights"]
    scores = [sum(w * x for w, x in zip(weights, row)) for row in rows]
    probabilities = softmax(scores) if scores else []

    if probabilities:
        best = max(range(len(probabilities)), key=lambda i: probabilities[i])
        confident = probabilities[best] >= artifact["abstain_below"]
    else:
        best = -1
        confident = False

    return {
        "name": case["name"],
        "features": rows,
        "scores": scores,
        "probabilities": probabilities,
        # The two things the runtime actually acts on.
        "chosen": best if confident else -1,
        "abstained": not confident,
    }


def default_cases():
    """Cases that cover the decisions and, more importantly, the near-misses.

    An exact match is easy for both sides to get right. What catches a drift is the
    cases where the two implementations could plausibly differ: a name that is a prefix
    of another, a role the observer could not determine, a candidate that affords
    nothing, and a single-candidate list where the position denominator would divide by
    zero if either side got the guard wrong.
    """
    fixture = [
        {"name": "Document", "role": "edit", "may_invoke": False, "may_edit": True},
        {"name": "Notes", "role": "edit", "may_invoke": False, "may_edit": True},
        {"name": "Save", "role": "button", "may_invoke": True, "may_edit": False},
        {"name": "Save as", "role": "button", "may_invoke": True, "may_edit": False},
        {"name": "Send", "role": "button", "may_invoke": True, "may_edit": False},
        {"name": "", "role": "", "may_invoke": False, "may_edit": False},
        # Three unlabelled fields that differ only in the panel they sit in and the
        # label UI Automation infers. Version 1 could not tell them apart at all, which
        # is precisely why it learned position; these cases exist so that a future
        # drift in the context features is caught here rather than in a trained model.
        {"name": "", "role": "edit", "container": "Compose", "inferred_label": "",
         "may_invoke": False, "may_edit": True},
        {"name": "", "role": "edit", "container": "Subject", "inferred_label": "",
         "may_invoke": False, "may_edit": True},
        {"name": "", "role": "edit", "container": "", "inferred_label": "Body",
         "may_invoke": False, "may_edit": True},
    ]
    return [
        {"name": "exact name, exact role",
         "subgoal": {"intent": "interact_with_control", "target_name": "Save",
                     "target_role": "button"},
         "candidates": fixture},
        {"name": "exact name, no role asked",
         "subgoal": {"intent": "interact_with_control", "target_name": "Save",
                     "target_role": ""},
         "candidates": fixture},
        {"name": "name is a prefix of another candidate",
         "subgoal": {"intent": "interact_with_control", "target_name": "Save as",
                     "target_role": "button"},
         "candidates": fixture},
        {"name": "payload entry",
         "subgoal": {"intent": "enter_payload", "target_name": "Document",
                     "target_role": "edit"},
         "candidates": fixture},
        {"name": "intent wants edit, target is a button",
         "subgoal": {"intent": "enter_payload", "target_name": "Save",
                     "target_role": "button"},
         "candidates": fixture},
        {"name": "nothing named",
         "subgoal": {"intent": "interact_with_control", "target_name": "",
                     "target_role": ""},
         "candidates": fixture},
        {"name": "no such control",
         "subgoal": {"intent": "interact_with_control", "target_name": "Publish",
                     "target_role": "button"},
         "candidates": fixture},
        {"name": "unnamed field, reached by its panel",
         "subgoal": {"intent": "enter_payload", "target_name": "",
                     "target_role": "edit", "target_container": "Compose"},
         "candidates": fixture},
        {"name": "unnamed field, reached by an inferred label",
         "subgoal": {"intent": "enter_payload", "target_name": "Body",
                     "target_role": "edit"},
         "candidates": fixture},
        {"name": "panel named that nothing sits in",
         "subgoal": {"intent": "enter_payload", "target_name": "",
                     "target_role": "edit", "target_container": "Attachments"},
         "candidates": fixture},
        {"name": "single candidate",
         "subgoal": {"intent": "interact_with_control", "target_name": "Save",
                     "target_role": "button"},
         "candidates": [fixture[2]]},
        {"name": "no candidates at all",
         "subgoal": {"intent": "interact_with_control", "target_name": "Save",
                     "target_role": "button"},
         "candidates": []},
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact", required=True)
    parser.add_argument("--cases", help="case file; the built-in set when omitted")
    parser.add_argument("--out", required=True, help="where to write Python's answers")
    parser.add_argument("--write-cases", help="also write the cases that were used")
    arguments = parser.parse_args()

    with open(arguments.artifact, "r", encoding="utf-8") as handle:
        artifact = json.load(handle)

    if artifact.get("feature_version") != feature_module.FEATURE_VERSION:
        print("The artifact was fitted on feature version %s; this build computes %d."
              % (artifact.get("feature_version"), feature_module.FEATURE_VERSION))
        return 1
    if artifact.get("feature_names") != feature_module.FEATURE_NAMES:
        print("The artifact's feature names are not the ones this build computes.")
        return 1

    if arguments.cases:
        with open(arguments.cases, "r", encoding="utf-8") as handle:
            cases = json.load(handle)
    else:
        cases = default_cases()

    if arguments.write_cases:
        with open(arguments.write_cases, "w", encoding="utf-8") as handle:
            json.dump(cases, handle, indent=2)
            handle.write("\n")

    expected = {
        "feature_version": feature_module.FEATURE_VERSION,
        "feature_names": feature_module.FEATURE_NAMES,
        "abstain_below": artifact["abstain_below"],
        "weights": artifact["weights"],
        "feature_tolerance": FEATURE_TOLERANCE,
        "score_tolerance": SCORE_TOLERANCE,
        "cases": [evaluate_case(artifact, case) for case in cases],
    }

    with open(arguments.out, "w", encoding="utf-8") as handle:
        json.dump(expected, handle, indent=2)
        handle.write("\n")

    print("wrote %d case(s) to %s" % (len(expected["cases"]), arguments.out))
    for case in expected["cases"]:
        print("  %-40s chosen=%-3d abstained=%s"
              % (case["name"], case["chosen"], case["abstained"]))
    print("\nNow run the deployment half:")
    print("  ReviaTests.exe --computer-parity %s %s"
          % (arguments.write_cases or "<cases.json>", arguments.out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
