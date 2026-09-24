#include "testSupport.h"

#include "Computer/learnedPolicy.h"
#include "Computer/subgoalValidator.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{

using namespace revia::computer;
using revia::tests::Check;

// What an artifact has to prove before it is allowed to decide anything.
//
// Every test below is a way a trained model goes wrong without looking wrong. None of
// them produce a crash or an obviously bad answer: they produce a policy that ranks
// confidently against the wrong columns, or in an application nobody measured it in, or
// with weights somebody edited after the numbers were taken. The only defence is
// refusing to load, which is what these check.

// The one canonical artifact, built the way the training tool builds one so the digest
// is real rather than asserted.
nlohmann::json GoodArtifact()
{
    // Weights that make "name_exact" decisive, which is what training on the controlled
    // domain actually produces.
    const std::vector<double> weights = {
        0.0, 5.0, 0.0, 0.0, 1.5, 0.0, 1.5, 0.0, 0.3, -0.9, 0.0, 0.0,
        // The context an unnamed control is reached by. Weighted like the name
        // features, because for a control with no name they are the name features.
        3.0, 1.0, 4.0, 1.0, 0.0, 0.0};
    const std::vector<std::string> names = {
        "bias", "name_exact", "name_prefix", "name_contains", "role_exact",
        "role_unspecified", "affords_intent", "affords_nothing_useful",
        "position_first", "position_normalised", "descriptor_named_control",
        "descriptor_named_role", "container_exact", "container_contains",
        "label_exact", "label_contains", "candidate_nameless",
        "descriptor_named_container"};
    const std::vector<std::string> applications = {"reviadesktopfixture.exe"};
    const std::vector<std::string> intents = {"interact_with_control", "enter_payload"};
    const double abstainBelow = 0.5;

    return {
        {"artifact_version", 1},
        {"feature_version", 2},
        {"feature_names", names},
        {"weights", weights},
        {"abstain_below", abstainBelow},
        {"qualified_scope", {
            {"applications", applications},
            {"intents", intents}}},
        // Enough evidence, and diverse enough, to be *eligible*.
        //
        // These numbers were 12 examples and nothing else until the qualification floors
        // were added. That artifact loaded, which is what the test was checking, and it
        // would also have loaded with all twelve examples drawn from one application in
        // one layout -- which is exactly how the first real artifact came to be trusted
        // enough to be compared and turned out to have learned screen positions.
        {"held_out", {
            {"examples", 60},
            {"coverage", 0.9},
            {"accuracy_on_covered", 1.0},
            {"wrong_and_confident", 0},
            {"applications", 3},
            {"task_families", 5},
            {"layouts", 2},
            {"sessions", 4},
            {"abstentions", 6}}},
        {"dataset_lineage", "fixture-lineage"},
        {"trained_at", "2026-09-20T00:00:00Z"},
        // Filled in by Write(), which computes it the way the runtime will.
        {"behaviour_hash", ""}};
}

// Writes an artifact with a correct behaviour hash for whatever it now contains, unless
// the caller asked for a broken one.
std::filesystem::path Write(
    const revia::tests::ScopedTestDirectory& directory,
    nlohmann::json artifact,
    const std::string& name = "artifact.json",
    const bool fixHash = true)
{
    if (fixHash)
    {
        // Recomputed from the artifact's own contents, so a test that changes a weight
        // gets a valid hash for the changed weight and is therefore testing the thing
        // it meant to test rather than the hash check.
        std::vector<std::string> names;
        for (const auto& entry : artifact["feature_names"]) names.push_back(entry);
        std::vector<double> weights;
        for (const auto& entry : artifact["weights"]) weights.push_back(entry);
        std::vector<std::string> applications;
        for (const auto& entry : artifact["qualified_scope"]["applications"])
            applications.push_back(entry);
        std::vector<std::string> intents;
        for (const auto& entry : artifact["qualified_scope"]["intents"])
            intents.push_back(entry);
        artifact["behaviour_hash"] = revia::computer::BehaviourDigestForTest(
            artifact.value("feature_version", 0u), names, weights,
            artifact.value("abstain_below", 1.0), applications, intents);
    }

    const auto path = directory.root / name;
    std::ofstream file(path);
    file << artifact.dump(2);
    return path;
}

ObservedCandidate Button(const std::string& name)
{
    ObservedCandidate candidate;
    candidate.id = name;
    candidate.name = name;
    candidate.role = "button";
    candidate.mayInvoke = true;
    return candidate;
}

ComputerSubgoal Validated(
    const std::string& application,
    const std::string& name,
    const SubgoalIntent intent,
    const PayloadVault& vault)
{
    ComputerSubgoal proposed;
    proposed.id = NewSubgoalId();
    proposed.intent = intent;
    proposed.target.application = application;
    proposed.target.name = name;
    proposed.target.role = "button";

    SubgoalContext context;
    context.goalId = "goal-1";
    context.origin = RequestOrigin::UserDirected;
    context.scope.approvedApplications = {application};
    context.scope.desktopControl.maxTypedCharacters = 512;

    const SubgoalValidation validation = ValidateSubgoal(proposed, context, vault);
    Check(validation.accepted, "The test's own subgoal was refused: " + validation.detail);
    return validation.subgoal;
}

ComputerTaskContext Screen(std::vector<ObservedCandidate> candidates)
{
    ComputerTaskContext context;
    context.observation.screen.succeeded = true;
    context.observation.screen.foregroundApplication = "reviadesktopfixture.exe";
    context.observation.candidates = std::move(candidates);
    return context;
}

// A good artifact loads, and says what it is.
void TestAQualifiedArtifactLoads()
{
    revia::tests::ScopedTestDirectory directory;
    const auto path = Write(directory, GoodArtifact());
    const ArtifactLoad load = LoadLearnedArtifact(path);
    Check(load.accepted, "A well-formed qualified artifact was refused: " + load.detail);
    Check(load.artifact.weights.size() == 18 && load.artifact.heldOutExamples == 60,
        "The artifact did not carry what it claims.");
    Check(!load.artifact.behaviourHash.empty(),
        "The artifact loaded without a behaviour hash to identify it by.");
}

// The refusals, each for its own reason.
// Zero confident mistakes on a narrow held-out set is not evidence, and the loader says
// so for each axis by name.
//
// Every one of these is a way the first trained artifact looked qualified and was not: a
// handful of examples, from one application, in one arrangement, in a single session.
void TestANarrowHeldOutSetIsRefusedOnEveryAxis()
{
    revia::tests::ScopedTestDirectory directory;
    const auto refusedFor = [&](const std::string& field, const nlohmann::json& value,
        const std::string& file)
    {
        nlohmann::json narrow = GoodArtifact();
        narrow["held_out"][field] = value;
        const ArtifactLoad load = LoadLearnedArtifact(Write(directory, narrow, file));
        Check(load.rejection == ArtifactRejection::NotQualified,
            "An artifact with " + field + " below the floor was accepted. Zero mistakes "
            "on a narrow set is not evidence that a policy generalises.");
        // The message spells the axis in words -- "task families" -- where the artifact
        // spells it with an underscore, so the field name is normalised before looking.
        std::string spoken = field;
        std::replace(spoken.begin(), spoken.end(), '_', ' ');
        Check(load.detail.find(spoken) != std::string::npos ||
                load.detail.find("abstains") != std::string::npos,
            "The refusal did not name the axis that was short, so nobody reading it "
            "knows what more evidence would look like: " + load.detail);
    };

    refusedFor("examples", 4, "few-examples.json");
    refusedFor("applications", 1, "one-application.json");
    refusedFor("task_families", 2, "two-families.json");
    refusedFor("layouts", 1, "one-layout.json");
    refusedFor("sessions", 1, "one-session.json");
    refusedFor("coverage", 0.1, "low-coverage.json");

    // And the artifact that meets every floor still loads, so the gate is a floor rather
    // than a refusal to accept anything.
    const ArtifactLoad good =
        LoadLearnedArtifact(Write(directory, GoodArtifact(), "eligible.json"));
    Check(good.accepted,
        "An artifact meeting every floor was refused: " + good.detail);
}

void TestAnArtifactMustProveItself()
{
    revia::tests::ScopedTestDirectory directory;

    Check(LoadLearnedArtifact(directory.root / "nothing.json").rejection ==
            ArtifactRejection::Missing,
        "A missing artifact was not reported as missing.");

    {
        const auto path = directory.root / "garbage.json";
        std::ofstream file(path);
        file << "this is not json";
        Check(LoadLearnedArtifact(path).rejection == ArtifactRejection::Malformed,
            "An unreadable artifact was not reported as malformed.");
    }

    {
        nlohmann::json future = GoodArtifact();
        future["feature_version"] = 99;
        Check(LoadLearnedArtifact(Write(directory, future, "future.json")).rejection ==
                ArtifactRejection::UnsupportedFeatureVersion,
            "An artifact fitted on features this build does not compute was accepted. "
            "Its weights are positional: applied to the wrong columns they are a "
            "different function, not a worse one.");
    }

    {
        nlohmann::json shuffled = GoodArtifact();
        // The same names in a different order. Every length check still passes.
        std::swap(shuffled["feature_names"][1], shuffled["feature_names"][2]);
        Check(LoadLearnedArtifact(Write(directory, shuffled, "shuffled.json")).rejection ==
                ArtifactRejection::FeatureNamesMismatch,
            "A reordered feature contract was accepted, which would land every weight "
            "on the wrong column silently.");
    }

    {
        nlohmann::json short_ = GoodArtifact();
        short_["weights"].erase(0);
        Check(LoadLearnedArtifact(Write(directory, short_, "short.json")).rejection ==
                ArtifactRejection::WeightCountMismatch,
            "An artifact with the wrong number of weights was accepted.");
    }

    {
        nlohmann::json unscoped = GoodArtifact();
        unscoped["qualified_scope"]["applications"] = nlohmann::json::array();
        Check(LoadLearnedArtifact(Write(directory, unscoped, "unscoped.json")).rejection ==
                ArtifactRejection::NoQualifiedScope,
            "An artifact that does not say what it was evaluated on was accepted, and "
            "there is nowhere it has been shown to work.");
    }

    {
        nlohmann::json untested = GoodArtifact();
        untested["held_out"]["examples"] = 0;
        Check(LoadLearnedArtifact(Write(directory, untested, "untested.json")).rejection ==
                ArtifactRejection::NotQualified,
            "An artifact never evaluated on unseen data was accepted.");
    }

    {
        nlohmann::json mistaken = GoodArtifact();
        mistaken["held_out"]["wrong_and_confident"] = 1;
        Check(LoadLearnedArtifact(Write(directory, mistaken, "mistaken.json")).rejection ==
                ArtifactRejection::NotQualified,
            "An artifact that was confidently wrong on held-out data was accepted. An "
            "abstention costs a model call; that costs an action nobody authorized.");
    }
}

// An artifact edited after it was evaluated is a different model wearing the old numbers.
void TestAnEditedArtifactIsRefused()
{
    revia::tests::ScopedTestDirectory directory;
    nlohmann::json artifact = GoodArtifact();
    const auto path = Write(directory, artifact, "good.json");
    Check(LoadLearnedArtifact(path).accepted, "The baseline artifact did not load.");

    // Somebody changes a weight and leaves the rest alone -- including the held-out
    // numbers that justified shipping it.
    nlohmann::json edited = GoodArtifact();
    edited["weights"][1] = 99.0;
    const auto editedPath = Write(directory, edited, "edited.json", /*fixHash=*/false);
    {
        // Give it back the ORIGINAL artifact's hash, which is exactly what an edit in
        // place would leave behind.
        std::ifstream in(path);
        const nlohmann::json original = nlohmann::json::parse(in);
        nlohmann::json tampered = edited;
        tampered["behaviour_hash"] = original["behaviour_hash"];
        std::ofstream out(editedPath);
        out << tampered.dump(2);
    }
    const ArtifactLoad load = LoadLearnedArtifact(editedPath);
    Check(load.rejection == ArtifactRejection::HashMismatch,
        "An artifact whose weights changed after evaluation was accepted on the "
        "strength of numbers that were measured on the old ones.");

    // And one with no hash at all, which is the same problem stated differently.
    nlohmann::json unhashed = GoodArtifact();
    unhashed["behaviour_hash"] = "";
    Check(LoadLearnedArtifact(
            Write(directory, unhashed, "unhashed.json", /*fixHash=*/false)).rejection ==
            ArtifactRejection::HashMismatch,
        "An artifact with nothing to identify its weights by was accepted.");
}

// A number measured in one application is not a number about another.
void TestAnArtifactRefusesWorkItWasNeverEvaluatedOn()
{
    revia::tests::ScopedTestDirectory directory;
    PayloadVault vault;
    LearnedComputerPolicy policy(vault);
    Check(policy.Load(Write(directory, GoodArtifact())).accepted,
        "The artifact did not load.");
    Check(policy.IsAvailable(), "A loaded artifact reported itself unavailable.");

    policy.SetSubgoal(Validated(
        "reviadesktopfixture.exe", "Save", SubgoalIntent::InteractWithControl, vault));
    const ComputerDecision inScope =
        policy.Decide(Screen({Button("Save"), Button("Send")}), {});
    Check(inScope.kind == ComputerDecisionKind::ProposeAction &&
            inScope.step.action.control == "Save",
        "A qualified artifact did not decide inside its own scope.");

    // The same artifact, the same screen, a different application.
    policy.SetSubgoal(Validated(
        "notepad.exe", "Save", SubgoalIntent::InteractWithControl, vault));
    const ComputerDecision outOfScope =
        policy.Decide(Screen({Button("Save"), Button("Send")}), {});
    Check(outOfScope.kind == ComputerDecisionKind::CannotHandle &&
            outOfScope.code == ComputerReasonCode::OutsideQualifiedScope,
        "An artifact decided in an application it was never evaluated in.");

    // And an intent it was not qualified for.
    policy.SetSubgoal(Validated(
        "reviadesktopfixture.exe", "Save", SubgoalIntent::FocusWindow, vault));
    Check(policy.Decide(Screen({Button("Save")}), {}).kind ==
            ComputerDecisionKind::CannotHandle,
        "An artifact decided on an intent it was never evaluated on.");
}

// Abstention is a designed outcome, not a failure.
void TestAnUnclearRankingAbstains()
{
    revia::tests::ScopedTestDirectory directory;
    PayloadVault vault;
    LearnedComputerPolicy policy(vault);
    Check(policy.Load(Write(directory, GoodArtifact())).accepted,
        "The artifact did not load.");
    policy.SetSubgoal(Validated(
        "reviadesktopfixture.exe", "Publish", SubgoalIntent::InteractWithControl, vault));

    // Nothing matches the name, so no candidate stands out and the softmax stays flat.
    const ComputerDecision decision = policy.Decide(
        Screen({Button("Save"), Button("Send"), Button("Cancel"), Button("Close")}), {});
    Check(decision.kind == ComputerDecisionKind::CannotHandle,
        "A ranking with no clear winner proposed an action anyway.");
    Check(decision.code == ComputerReasonCode::OutsideQualifiedScope,
        "The abstention was not reported as one.");
}

// Without an artifact it is unavailable, and never asked.
void TestAnUnloadedPolicyIsUnavailable()
{
    PayloadVault vault;
    LearnedComputerPolicy policy(vault);
    Check(!policy.IsAvailable(),
        "A policy with no artifact reported itself ready to decide.");
    const ComputerDecision decision = policy.Decide(Screen({Button("Save")}), {});
    Check(decision.kind == ComputerDecisionKind::CannotHandle &&
            decision.code == ComputerReasonCode::ProviderUnavailable,
        "A policy with no artifact produced something other than 'unavailable'.");
}

// The features are the ones the training tool computes, checked against values worked
// out by hand rather than against the implementation being tested.
void TestTheFeatureContractIsWhatItSaysItIs()
{
    PayloadVault vault;
    ComputerSubgoal subgoal = Validated(
        "reviadesktopfixture.exe", "Save", SubgoalIntent::InteractWithControl, vault);

    const std::vector<double> exact =
        CandidateFeatures(subgoal, Button("Save"), 0, 4);
    Check(exact.size() == 18, "The feature vector is not the width the artifact expects.");
    Check(exact[0] == 1.0, "The bias is not one.");
    Check(exact[1] == 1.0 && exact[2] == 0.0 && exact[3] == 0.0,
        "An exact name match did not read as exact, and only as exact.");
    Check(exact[4] == 1.0 && exact[5] == 0.0, "The role match is wrong.");
    Check(exact[6] == 1.0, "A button did not afford an interaction.");
    Check(exact[8] == 1.0 && exact[9] == 0.0, "The position features are wrong at zero.");

    // "Save as" starts with "Save", which is the case a looser matcher gets wrong.
    const std::vector<double> prefix =
        CandidateFeatures(subgoal, Button("Save as"), 1, 4);
    Check(prefix[1] == 0.0 && prefix[2] == 1.0 && prefix[3] == 0.0,
        "A prefix match was recorded as an exact one.");
    Check(prefix[8] == 0.0 && std::abs(prefix[9] - (1.0 / 3.0)) < 1e-12,
        "The normalised position is wrong.");

    // One candidate: the denominator would be zero if the guard were missing.
    const std::vector<double> alone = CandidateFeatures(subgoal, Button("Save"), 0, 1);
    Check(alone[9] == 0.0, "A single-candidate list produced a bad position feature.");

    // The context features, which version 1 did not have and whose absence is why the
    // first trained ranker learned where things sat instead of what they were called.
    // Built directly rather than through validation: CandidateFeatures is a pure
    // function of the descriptor, and a subgoal that has not been stamped is exactly
    // what it is being asked about here.
    ComputerSubgoal byContainer;
    byContainer.intent = SubgoalIntent::EnterPayload;
    byContainer.target.application = "reviadesktopfixture.exe";
    byContainer.target.container = "Compose";
    byContainer.target.role = "edit";

    ObservedCandidate unnamed;
    unnamed.id = "edit-1";
    unnamed.role = "edit";
    unnamed.container = "Compose";
    unnamed.nameless = true;
    unnamed.maySetText = true;
    const std::vector<double> inPanel = CandidateFeatures(byContainer, unnamed, 0, 3);
    Check(inPanel[12] == 1.0, "A candidate in the named panel did not match its container.");
    Check(inPanel[16] == 1.0, "A candidate with no name was not marked nameless.");
    Check(inPanel[17] == 1.0, "The descriptor named a container and the row did not say so.");

    ObservedCandidate elsewhere = unnamed;
    elsewhere.id = "edit-2";
    elsewhere.container = "Attachments";
    const std::vector<double> outside = CandidateFeatures(byContainer, elsewhere, 1, 3);
    Check(outside[12] == 0.0 && outside[13] == 0.0,
        "A candidate in a different panel matched the container anyway, which would "
        "make the feature that distinguishes two unlabelled fields useless.");
}

} // namespace

void RunLearnedPolicyTests()
{
    TestAQualifiedArtifactLoads();
    TestANarrowHeldOutSetIsRefusedOnEveryAxis();
    TestAnArtifactMustProveItself();
    TestAnEditedArtifactIsRefused();
    TestAnArtifactRefusesWorkItWasNeverEvaluatedOn();
    TestAnUnclearRankingAbstains();
    TestAnUnloadedPolicyIsUnavailable();
    TestTheFeatureContractIsWhatItSaysItIs();

    std::cout << "A learned artifact has to prove its features, its scope and its "
                 "evidence before it decides anything.\n";
}
