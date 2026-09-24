"""Features for the bounded candidate ranker, in one place.

This file is the contract between training and deployment. The C++ side
(`Private/Computer/learnedPolicy.cpp`) computes the same numbers from the same
observation, and `parity.py` checks that it does -- a ranker whose features drift
between the two is a ranker that was evaluated on one problem and deployed on another.

Two rules govern what may appear here, and both are about what the policy would be
learning if they were broken.

Nothing that identifies this machine. No automation ids, no runtime ids, no window
handles, no process ids, no absolute paths. Those are temporary handles for one window
on one desktop today; a weight learned against "1002" is a weight that has learned this
fixture rather than the idea of a document field.

Nothing that is only true of this dataset. Every feature below is a relation between the
*subgoal* and a *candidate* -- does the name match, does the role match, does it afford
what the intent needs. A feature that read only the candidate would let the model learn
"press whatever is called Save", which is not a policy, it is a habit.
"""

from __future__ import annotations

# Bumped whenever the meaning or order of FEATURE_NAMES changes. An artifact carries the
# version it was trained under, and a runtime that does not implement that version
# refuses the artifact rather than scoring with the wrong columns in the wrong places.
# 1 -- name, role, affordance, position, descriptor specificity.
# 2 -- adds the context an unnamed control is identified by: the panel it sits in and
#      the label UI Automation infers for it, plus an explicit "this control publishes
#      no name" indicator.
#
#      Version 1 was measurably incomplete and the measurement is worth recording. The
#      only rows that survived validation were payload entries into unnamed fields --
#      where `name_exact` is 0 for every candidate by construction -- so the single
#      column that varied across candidates was `position_normalised`. The ranker did
#      exactly what the design matrix permitted: it learned position (+5.59) and
#      nothing about names (0.00), and calibration correctly refused to let it act.
#      That is not a training failure. It is a feature set that could not express the
#      problem the dataset was made of.
FEATURE_VERSION = 2

FEATURE_NAMES = [
    "bias",
    # The name, compared three ways. Exact is the one that should carry the weight;
    # the looser two exist so the model can learn how much less they are worth rather
    # than having that decided for it.
    "name_exact",
    "name_prefix",
    "name_contains",
    # The role the subgoal asked for, when it asked for one.
    "role_exact",
    "role_unspecified",
    # What the candidate can do, against what the intent needs. A candidate that cannot
    # do the thing is not a near miss, it is not a candidate.
    "affords_intent",
    "affords_nothing_useful",
    # Where it sits in the list. A weak prior and deliberately a small one: position is
    # informative in a toolbar and meaningless in a form, and a model that leaned on it
    # would be learning this fixture's layout.
    "position_first",
    "position_normalised",
    # How specific the description was. A subgoal that named nothing is a subgoal whose
    # match is a guess, and the model should be able to learn to abstain on it.
    "descriptor_named_control",
    "descriptor_named_role",
    # The context an unnamed control is reached by.
    #
    # A plain EDIT with nothing beside it publishes no name, so every name feature above
    # is zero for it and always will be. What distinguishes it from the box next to it
    # is the panel it sits in and the label UI Automation infers -- and until these
    # existed, a model asked to choose between three unnamed fields had nothing to look
    # at but where they were in the list.
    "container_exact",
    "container_contains",
    "label_exact",
    "label_contains",
    # Whether this control publishes a name at all. Not a proxy for the above: it says
    # that the name features are uninformative for this row rather than negative, which
    # is a different thing for a linear model to know.
    "candidate_nameless",
    # Whether the subgoal named a container. The same question `descriptor_named_control`
    # asks about the name, for the evidence that replaces it.
    "descriptor_named_container",
]

INTENT_NEEDS_EDIT = {"enter_payload"}
INTENT_NEEDS_INVOKE = {"interact_with_control"}


def _lower(value):
    return (value or "").strip().lower()


def candidate_features(subgoal, candidate, index, candidate_count):
    """One row of the design matrix: this candidate, for this subgoal.

    `subgoal` is a dict with `intent`, `target_name`, `target_role` and
    `target_container`.
    `candidate` is a dict with `name`, `role`, `container`, `inferred_label`,
    `may_invoke` and `may_edit`.
    """
    wanted_name = _lower(subgoal.get("target_name"))
    wanted_role = _lower(subgoal.get("target_role"))
    wanted_container = _lower(subgoal.get("target_container"))
    name = _lower(candidate.get("name"))
    role = _lower(candidate.get("role"))
    container = _lower(candidate.get("container"))
    label = _lower(candidate.get("inferred_label"))
    intent = _lower(subgoal.get("intent"))

    exact = 1.0 if wanted_name and name == wanted_name else 0.0
    prefix = 1.0 if wanted_name and not exact and name.startswith(wanted_name) else 0.0
    contains = 1.0 if wanted_name and not exact and not prefix and wanted_name in name else 0.0

    role_exact = 1.0 if wanted_role and role == wanted_role else 0.0
    role_unspecified = 1.0 if not wanted_role else 0.0

    may_edit = bool(candidate.get("may_edit"))
    may_invoke = bool(candidate.get("may_invoke"))
    if intent in INTENT_NEEDS_EDIT:
        affords = 1.0 if may_edit else 0.0
    elif intent in INTENT_NEEDS_INVOKE:
        affords = 1.0 if may_invoke else 0.0
    else:
        affords = 1.0 if (may_edit or may_invoke) else 0.0
    affords_nothing = 0.0 if (may_edit or may_invoke) else 1.0

    container_exact = 1.0 if wanted_container and container == wanted_container else 0.0
    container_contains = (
        1.0
        if wanted_container and not container_exact and wanted_container in container
        else 0.0
    )
    # The inferred label is compared against the name the subgoal asked for, not against
    # a separate field. A user names a field by whatever they can read beside it, and
    # for an unlabelled box that is exactly what UI Automation infers.
    label_exact = 1.0 if wanted_name and label == wanted_name else 0.0
    label_contains = (
        1.0 if wanted_name and not label_exact and wanted_name in label else 0.0
    )
    nameless = 1.0 if not name else 0.0

    denominator = max(candidate_count - 1, 1)
    return [
        1.0,
        exact,
        prefix,
        contains,
        role_exact,
        role_unspecified,
        affords,
        affords_nothing,
        1.0 if index == 0 else 0.0,
        index / denominator,
        1.0 if wanted_name else 0.0,
        1.0 if wanted_role else 0.0,
        container_exact,
        container_contains,
        label_exact,
        label_contains,
        nameless,
        1.0 if wanted_container else 0.0,
    ]


def design_matrix(subgoal, candidates):
    """Every candidate's features, in the order the observation listed them.

    Order matters and is preserved deliberately: the C++ side scores the same list in
    the same order, and `parity.py` compares argmax. A ranker that silently sorted here
    would disagree with deployment on every tie.
    """
    count = len(candidates)
    return [
        candidate_features(subgoal, candidate, index, count)
        for index, candidate in enumerate(candidates)
    ]
