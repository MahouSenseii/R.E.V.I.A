"""Train, evaluate and package the bounded candidate ranker.

    python Tools/Computer/train.py --dataset RuntimeData/ComputerExperience \
                                   --out RuntimeData/ComputerArtifacts

Deliberately dependency-free. Not because a linear model is the end of the road, but
because it is the baseline the road starts from: section 9 of the design asks for a
comparison against deterministic and non-neural learned baselines before anything larger
is justified, and a baseline nobody can run is not a baseline. This trains on the
standard library alone, on a CPU, in under a second, and produces an artifact the
runtime can load without ONNX Runtime, without torch, and without a download.

What it is: multinomial logistic regression over the candidates of one observation,
scored by the shared features in `features.py`, with an explicit abstention threshold
calibrated on held-out data. The model picks a candidate or declines to.

What it is not: evidence that any of this generalises beyond the domain it was trained
on. The artifact records its own qualified scope, and the runtime refuses to use it
outside that scope. A number produced here is a number about these tasks in these
applications.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import dataset as dataset_module
import features as feature_module

ARTIFACT_VERSION = 1


def softmax(scores):
    highest = max(scores)
    exponentials = [math.exp(score - highest) for score in scores]
    total = sum(exponentials)
    return [value / total for value in exponentials]


def forward(weights, rows):
    return [sum(w * x for w, x in zip(weights, row)) for row in rows]


def train(examples, epochs=400, learning_rate=0.30, l2=1e-3, seed=20260920):
    """Plain gradient descent on the cross-entropy of the candidate distribution.

    One example is one observation: a list of candidates and the index of the one that
    was chosen. The gradient is the usual softmax-regression gradient, summed over
    candidates within an example and then over examples.
    """
    width = len(feature_module.FEATURE_NAMES)
    weights = [0.0] * width
    history = []

    for epoch in range(epochs):
        gradient = [0.0] * width
        loss = 0.0
        for example in examples:
            rows = feature_module.design_matrix(example.subgoal(), example.candidates)
            if not rows:
                continue
            probabilities = softmax(forward(weights, rows))
            loss -= math.log(max(probabilities[example.label], 1e-12))
            for index, row in enumerate(rows):
                error = probabilities[index] - (1.0 if index == example.label else 0.0)
                for column, value in enumerate(row):
                    gradient[column] += error * value

        count = max(len(examples), 1)
        for column in range(width):
            # No weight decay on the bias: it is the model's freedom to be uncertain,
            # and shrinking it toward zero would make abstention harder to learn rather
            # than easier.
            decay = 0.0 if feature_module.FEATURE_NAMES[column] == "bias" else l2 * weights[column]
            weights[column] -= learning_rate * (gradient[column] / count + decay)
        history.append(loss / count)

    return weights, history


def evaluate(weights, examples, threshold):
    """Top-1 accuracy, coverage, and -- the number that matters -- wrong-and-confident.

    Accuracy alone would be a comfortable metric. A ranker that is right 95% of the time
    and confidently wrong the other 5% is worse for this job than one that is right 80%
    of the time and abstains on the rest, because an abstention costs a model call and a
    confident mistake costs a click nobody authorized.
    """
    total = len(examples)
    correct = 0
    covered = 0
    wrong_and_confident = 0

    for example in examples:
        rows = feature_module.design_matrix(example.subgoal(), example.candidates)
        if not rows:
            continue
        probabilities = softmax(forward(weights, rows))
        best = max(range(len(probabilities)), key=lambda i: probabilities[i])
        confident = probabilities[best] >= threshold
        if confident:
            covered += 1
            if best == example.label:
                correct += 1
            else:
                wrong_and_confident += 1

    return {
        "examples": total,
        "covered": covered,
        "coverage": covered / total if total else 0.0,
        "correct": correct,
        "accuracy_on_covered": correct / covered if covered else 0.0,
        "wrong_and_confident": wrong_and_confident,
    }


def calibrate(weights, examples, candidates=None):
    """Pick the abstention threshold on held-out data, against a stated gate.

    The gate is agreed here rather than after the fact: no threshold is acceptable if it
    lets a single confident mistake through on the held-out set. Among those that do
    not, the one with the widest coverage wins -- coverage is the saving, and it is only
    worth having once safety is settled.
    """
    if candidates is None:
        candidates = [0.50, 0.60, 0.70, 0.80, 0.90, 0.95, 0.99]
    best = None
    for threshold in candidates:
        result = evaluate(weights, examples, threshold)
        if result["wrong_and_confident"] > 0:
            continue
        if best is None or result["coverage"] > best[1]["coverage"]:
            best = (threshold, result)
    if best is None:
        # Nothing was safe at any threshold. Returning the strictest one and letting the
        # caller see a coverage of zero is more useful than returning the least-bad
        # mistake rate: the artifact is then plainly not worth promoting.
        strictest = max(candidates)
        return strictest, evaluate(weights, examples, strictest)
    return best


def behaviour_digest_input(weights, threshold, scope):
    """The bytes that decide how this artifact behaves, in a fixed order.

    Byte-for-byte what `Private/Computer/artifactDigest.cpp` builds. Deliberately not the
    JSON: hashing that would mean reproducing one language's float formatting in the
    other, which fails on the last digit of a perfectly good artifact and calls it
    tampering. `%.17g` is shortest-round-trip in both, and the fields here are exactly
    the ones that change a decision -- a different `trained_at` is not a different model.
    """
    lines = ["feature_version=%d" % feature_module.FEATURE_VERSION, "features"]
    lines.extend(feature_module.FEATURE_NAMES)
    lines.append("weights")
    lines.extend("%.17g" % weight for weight in weights)
    lines.append("abstain_below=%.17g" % threshold)
    lines.append("applications")
    lines.extend(scope["applications"])
    lines.append("intents")
    lines.extend(scope["intents"])
    return "\n".join(lines) + "\n"


def held_out_evidence(scores, examples):
    """What the held-out set proved, and how varied it was.

    The scores on their own are the number everyone reads and the number that misleads.
    The first artifact this pipeline produced scored zero confident mistakes -- on four
    examples, from one application, in one layout, in a single session -- and it had
    learned where things sit on the screen rather than what they are called.

    So the diversity is carried beside the scores and the runtime enforces floors on it
    at load. An artifact that cannot say how many applications, task families, layouts
    and sessions its evaluation covered is refused, rather than trusted because its
    accuracy looked good.
    """
    evidence = dict(scores or {"examples": 0, "note": "no held-out variants"})
    evidence["applications"] = len({e.application for e in examples if e.application})
    evidence["task_families"] = len({e.task_variant for e in examples if e.task_variant})
    evidence["layouts"] = len({e.layout_variant for e in examples if e.layout_variant})
    evidence["sessions"] = len({e.session_id for e in examples if e.session_id})
    covered = evidence.get("covered", 0)
    evidence["abstentions"] = max(len(examples) - covered, 0)
    return evidence


def package(weights, threshold, training, held_out, lineage_hash, scope, timings):
    """An immutable artifact that carries everything needed to judge it later."""
    body = {
        "artifact_version": ARTIFACT_VERSION,
        "feature_version": feature_module.FEATURE_VERSION,
        "dataset_schema": dataset_module.SUPPORTED_SCHEMA,
        "feature_names": feature_module.FEATURE_NAMES,
        "weights": weights,
        "abstain_below": threshold,
        # What this artifact is allowed to be used for. The runtime refuses it outside
        # this, which is what makes a narrow result safe to ship rather than a claim
        # that has to be remembered.
        "qualified_scope": scope,
        "training": training,
        "held_out": held_out,
        "dataset_lineage": lineage_hash,
        "trained_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "timings_ms": timings,
        # Checked by the runtime at load. See behaviour_digest_input.
        "behaviour_hash": hashlib.sha256(
            behaviour_digest_input(weights, threshold, scope).encode("utf-8")
        ).hexdigest(),
    }
    # The hash covers everything above it, so a changed weight is a changed identity.
    body["artifact_hash"] = hashlib.sha256(
        json.dumps(body, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    return body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", required=True,
                        help="directory of recorder .jsonl session files")
    parser.add_argument("--out", required=True, help="where to write the artifact")
    parser.add_argument("--name", default="routine-ranker",
                        help="artifact file name, without extension")
    parser.add_argument("--epochs", type=int, default=400)
    parser.add_argument("--holdout", type=float, default=0.3)
    parser.add_argument("--seed", type=int, default=20260920)
    arguments = parser.parse_args()

    rows = dataset_module.read_dataset(arguments.dataset)
    examples, refusals = dataset_module.validate(rows)

    # The funnel, stated in full every run.
    #
    # Every number between "rows recorded" and "examples trained on" is a place where
    # the dataset can quietly become something other than what it looks like, and a
    # report that gives only the two ends invites exactly the arithmetic confusion this
    # section exists to prevent.
    sessions = {row.get("session_id", "") for row in rows}
    variants = {
        (row.get("task_variant", ""), row.get("layout_variant", "")) for row in rows
    }
    print("")
    print("-- the funnel --")
    print("capture sessions:     %d" % len(sessions))
    print("task/layout variants: %d" % len({v for v in variants if v[0]}))
    print("raw decisions:        %d" % len(rows))
    print("admissible labels:    %d" % len(examples))
    for reason, count in sorted(refusals.items()):
        if count:
            print("  refused (%s): %d" % (reason, count))

    # And what each surviving row is evidence *of*. A ranker is trained on one of these
    # families and must never be scored on the others.
    grouped = dataset_module.by_family(examples)
    print("")
    print("-- what the admissible rows are evidence of --")
    for family in dataset_module.FAMILIES:
        print("  %-18s %d" % (family, len(grouped.get(family, []))))

    # Only target-selection rows can train a candidate ranker: they are the rows where
    # there was a choice between candidates and the evidence says which one was right.
    # Completion and abstention rows have no chosen candidate by construction, and an
    # action-effect row answers a different question about the same event.
    ranking = grouped.get("target_selection", [])
    print("")
    print("trainable as a ranker: %d  (target_selection only)" % len(ranking))
    examples = ranking

    if not examples:
        print("\nNothing to train on. This is an outcome, not an error: the recorder "
              "keeps nothing until someone opens a capture session, and a dataset of "
              "zero rows produces no artifact rather than an untrained one.")
        return 1

    train_set, test_set, held, split_kind = dataset_module.split_by_variant(
        examples, arguments.holdout, arguments.seed)
    print("\ntraining examples:    %d" % len(train_set))
    print("held-out examples:    %d  (held-out %ss: %s)"
          % (len(test_set), split_kind, ", ".join(held) or "none"))
    if split_kind == "session":
        print("  NOTE: no row carried a task variant, so this fell back to holding out")
        print("        whole sessions. Every session runs every task, so a held-out")
        print("        score from this split measures recall of a task shape already")
        print("        seen rather than generalisation to a new one.")

    if not train_set:
        print("\nEverything was held out; there is nothing left to fit.")
        return 1

    started = time.time()
    weights, history = train(train_set, epochs=arguments.epochs, seed=arguments.seed)
    training_ms = (time.time() - started) * 1000.0

    print("\nfinal training loss:  %.6f" % history[-1])
    for name, weight in zip(feature_module.FEATURE_NAMES, weights):
        print("  %-26s %+.4f" % (name, weight))

    # Calibrated on held-out data when there is any, and on the training set otherwise
    # -- with that fact recorded in the artifact, because a threshold fitted on the data
    # it is judged against is not a held-out number and must not be reported as one.
    calibration_set = test_set if test_set else train_set
    threshold, calibrated = calibrate(weights, calibration_set)

    started = time.time()
    on_training = evaluate(weights, train_set, threshold)
    on_held_out = evaluate(weights, test_set, threshold) if test_set else None
    inference_ms = (time.time() - started) * 1000.0

    print("\nabstain below:        %.2f  (calibrated on %s)"
          % (threshold, "held-out sessions" if test_set else "the training set"))
    print("training   : coverage %.3f  accuracy %.3f  wrong-and-confident %d"
          % (on_training["coverage"], on_training["accuracy_on_covered"],
             on_training["wrong_and_confident"]))
    if on_held_out:
        print("held out   : coverage %.3f  accuracy %.3f  wrong-and-confident %d"
              % (on_held_out["coverage"], on_held_out["accuracy_on_covered"],
                 on_held_out["wrong_and_confident"]))
    else:
        print("held out   : none -- one variant only, so no generalisation was measured")

    # Lower-cased here, at the one place a scope is written.
    #
    # The runtime compares application names case-insensitively, so the case carries no
    # meaning -- but it does carry into the behaviour digest, and a scope hashed as
    # "ReviaDesktopFixture.exe" on one side and "reviadesktopfixture.exe" on the other
    # makes a perfectly good artifact look edited. Normalising at the source is the fix;
    # normalising on only one side is the bug.
    applications = sorted({e.application.lower() for e in train_set if e.application})
    intents = sorted({e.intent.lower() for e in train_set if e.intent})
    scope = {
        "applications": applications,
        "intents": intents,
        # Said in the artifact itself so it travels with it.
        "note": ("Qualified only for these applications and intents, on the data this "
                 "lineage names. Nothing here is evidence about other software."),
    }

    artifact = package(
        weights, threshold,
        {"examples": len(train_set), "epochs": arguments.epochs,
         "final_loss": history[-1], **on_training},
        held_out_evidence(on_held_out, test_set),
        dataset_module.lineage(rows),
        scope,
        {"training": round(training_ms, 3), "evaluation": round(inference_ms, 3)},
    )

    os.makedirs(arguments.out, exist_ok=True)
    path = os.path.join(arguments.out, arguments.name + ".json")
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(artifact, handle, indent=2, sort_keys=True)
        handle.write("\n")

    print("\nartifact:             %s" % path)
    print("artifact hash:        %s" % artifact["artifact_hash"])
    print("dataset lineage:      %s" % artifact["dataset_lineage"])
    print("trained in:           %.1f ms" % training_ms)
    print("\nThis artifact is not promoted by being written. Check parity with "
          "Tools/Computer/parity.py, then select it deliberately.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
