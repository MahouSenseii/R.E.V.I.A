#include "Computer/learnedPolicy.h"

#include "artifactDigest.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>

namespace revia::computer
{

namespace
{

// Bumped together with Tools/Computer/features.py. A mismatch is refused rather than
// tolerated: the weights are positional, so applying a vector fitted under one feature
// order to another order is not degraded accuracy, it is a different function.
constexpr std::uint32_t SupportedArtifactVersion = 1;
// 2 adds the container, inferred label and namelessness columns. An artifact fitted
// under version 1 is refused rather than padded: the weights are positional, and a
// vector six columns short is not a slightly worse ranker, it is a different function.
constexpr std::uint32_t SupportedFeatureVersion = 2;

// The feature names, in order, exactly as features.py lists them. Compared rather than
// assumed, so a reordering upstream is caught at load instead of scoring silently wrong.
const std::vector<std::string>& ExpectedFeatureNames()
{
    static const std::vector<std::string> names = {
        "bias",
        "name_exact",
        "name_prefix",
        "name_contains",
        "role_exact",
        "role_unspecified",
        "affords_intent",
        "affords_nothing_useful",
        "position_first",
        "position_normalised",
        "descriptor_named_control",
        "descriptor_named_role",
        "container_exact",
        "container_contains",
        "label_exact",
        "label_contains",
        "candidate_nameless",
        "descriptor_named_container",
    };
    return names;
}

std::string Lowered(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string Trimmed(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

ArtifactLoad Refuse(const ArtifactRejection rejection, std::string detail)
{
    ArtifactLoad load;
    load.rejection = rejection;
    load.detail = std::move(detail);
    return load;
}

ComputerDecision Answer(
    const ComputerDecisionKind kind, const ComputerReasonCode code, std::string detail)
{
    ComputerDecision decision;
    decision.kind = kind;
    decision.code = code;
    decision.detail = std::move(detail);
    decision.provider = "learned";
    decision.tokens = 0;
    decision.costReported = true;
    return decision;
}

std::vector<double> Softmax(const std::vector<double>& scores)
{
    if (scores.empty()) return {};
    const double highest = *std::max_element(scores.begin(), scores.end());
    std::vector<double> exponentials;
    exponentials.reserve(scores.size());
    double total = 0.0;
    for (const double score : scores)
    {
        const double value = std::exp(score - highest);
        exponentials.push_back(value);
        total += value;
    }
    for (double& value : exponentials) value /= total;
    return exponentials;
}

} // namespace

std::string ToString(const ArtifactRejection value)
{
    switch (value)
    {
        case ArtifactRejection::Missing: return "missing";
        case ArtifactRejection::Malformed: return "malformed";
        case ArtifactRejection::UnsupportedArtifactVersion:
            return "unsupported_artifact_version";
        case ArtifactRejection::UnsupportedFeatureVersion:
            return "unsupported_feature_version";
        case ArtifactRejection::WeightCountMismatch: return "weight_count_mismatch";
        case ArtifactRejection::FeatureNamesMismatch: return "feature_names_mismatch";
        case ArtifactRejection::HashMismatch: return "hash_mismatch";
        case ArtifactRejection::NonFiniteWeight: return "non_finite_weight";
        case ArtifactRejection::NoQualifiedScope: return "no_qualified_scope";
        case ArtifactRejection::NotQualified: return "not_qualified";
        case ArtifactRejection::None: break;
    }
    return "none";
}

std::vector<double> CandidateFeatures(
    const ComputerSubgoal& subgoal,
    const ObservedCandidate& candidate,
    const std::size_t index,
    const std::size_t candidateCount)
{
    // Mirrors Tools/Computer/features.py::candidate_features exactly, including the
    // order. See that file for why each feature is what it is -- this is the deployment
    // half of one definition, not a second definition.
    const std::string wantedName = Lowered(Trimmed(subgoal.target.name));
    const std::string wantedRole = Lowered(Trimmed(subgoal.target.role));
    const std::string wantedContainer = Lowered(Trimmed(subgoal.target.container));
    const std::string name = Lowered(Trimmed(candidate.name));
    const std::string role = Lowered(Trimmed(candidate.role));
    const std::string container = Lowered(Trimmed(candidate.container));
    const std::string label = Lowered(Trimmed(candidate.inferredLabel));

    const double exact = (!wantedName.empty() && name == wantedName) ? 1.0 : 0.0;
    const double prefix = (!wantedName.empty() && exact == 0.0 &&
        name.rfind(wantedName, 0) == 0) ? 1.0 : 0.0;
    const double contains = (!wantedName.empty() && exact == 0.0 && prefix == 0.0 &&
        name.find(wantedName) != std::string::npos) ? 1.0 : 0.0;

    const double roleExact = (!wantedRole.empty() && role == wantedRole) ? 1.0 : 0.0;
    const double roleUnspecified = wantedRole.empty() ? 1.0 : 0.0;

    const bool mayEdit = candidate.maySetText || candidate.mayType;
    const bool mayInvoke = candidate.mayInvoke;
    double affords = 0.0;
    if (subgoal.intent == SubgoalIntent::EnterPayload)
    {
        affords = mayEdit ? 1.0 : 0.0;
    }
    else if (subgoal.intent == SubgoalIntent::InteractWithControl)
    {
        affords = mayInvoke ? 1.0 : 0.0;
    }
    else
    {
        affords = (mayEdit || mayInvoke) ? 1.0 : 0.0;
    }
    const double affordsNothing = (mayEdit || mayInvoke) ? 0.0 : 1.0;

    // The context an unnamed control is reached by. Without these the design matrix
    // could not express the only rows the dataset actually had, and the ranker learned
    // position because position was the only column that varied.
    const double containerExact =
        (!wantedContainer.empty() && container == wantedContainer) ? 1.0 : 0.0;
    const double containerContains = (!wantedContainer.empty() &&
        containerExact == 0.0 &&
        container.find(wantedContainer) != std::string::npos) ? 1.0 : 0.0;
    const double labelExact =
        (!wantedName.empty() && label == wantedName) ? 1.0 : 0.0;
    const double labelContains = (!wantedName.empty() && labelExact == 0.0 &&
        label.find(wantedName) != std::string::npos) ? 1.0 : 0.0;
    const double nameless = name.empty() ? 1.0 : 0.0;

    const double denominator = static_cast<double>(
        candidateCount > 1 ? candidateCount - 1 : 1);

    return {
        1.0,
        exact,
        prefix,
        contains,
        roleExact,
        roleUnspecified,
        affords,
        affordsNothing,
        index == 0 ? 1.0 : 0.0,
        static_cast<double>(index) / denominator,
        wantedName.empty() ? 0.0 : 1.0,
        wantedRole.empty() ? 0.0 : 1.0,
        containerExact,
        containerContains,
        labelExact,
        labelContains,
        nameless,
        wantedContainer.empty() ? 0.0 : 1.0,
    };
}

std::string BehaviourDigestForTest(
    const std::uint32_t featureVersion,
    const std::vector<std::string>& featureNames,
    const std::vector<double>& weights,
    const double abstainBelow,
    const std::vector<std::string>& applications,
    const std::vector<std::string>& intents)
{
    return Sha256Hex(BehaviourDigestInput(
        featureVersion, featureNames, weights, abstainBelow, applications, intents));
}

ArtifactLoad LoadLearnedArtifact(const std::filesystem::path& path)
{
    std::error_code error;
    if (path.empty() || !std::filesystem::exists(path, error) || error)
    {
        return Refuse(ArtifactRejection::Missing,
            "No learned artifact at " + path.string() + ".");
    }

    std::ifstream file(path);
    if (!file)
    {
        return Refuse(ArtifactRejection::Missing,
            "The learned artifact could not be opened.");
    }
    const nlohmann::json body = nlohmann::json::parse(file, nullptr, false);
    if (body.is_discarded() || !body.is_object())
    {
        return Refuse(ArtifactRejection::Malformed,
            "The learned artifact is not a readable JSON object.");
    }

    LearnedArtifact artifact;
    artifact.artifactVersion = body.value("artifact_version", 0u);
    artifact.featureVersion = body.value("feature_version", 0u);
    artifact.hash = body.value("artifact_hash", std::string{});
    artifact.datasetLineage = body.value("dataset_lineage", std::string{});
    artifact.trainedAt = body.value("trained_at", std::string{});
    artifact.abstainBelow = body.value("abstain_below", 1.0);

    if (artifact.artifactVersion != SupportedArtifactVersion)
    {
        return Refuse(ArtifactRejection::UnsupportedArtifactVersion,
            "The artifact was packaged by a version this build does not implement.");
    }
    if (artifact.featureVersion != SupportedFeatureVersion)
    {
        // The weights are positional. Applying them to a different feature order is not
        // a degraded model, it is a different function that happens to run.
        return Refuse(ArtifactRejection::UnsupportedFeatureVersion,
            "The artifact was fitted on a feature set this build does not implement.");
    }

    if (body.contains("feature_names") && body["feature_names"].is_array())
    {
        for (const auto& entry : body["feature_names"])
        {
            if (entry.is_string()) artifact.featureNames.push_back(entry.get<std::string>());
        }
    }
    if (artifact.featureNames != ExpectedFeatureNames())
    {
        return Refuse(ArtifactRejection::FeatureNamesMismatch,
            "The artifact's features are not the ones this build computes.");
    }

    if (body.contains("weights") && body["weights"].is_array())
    {
        for (const auto& entry : body["weights"])
        {
            if (entry.is_number()) artifact.weights.push_back(entry.get<double>());
        }
    }
    if (artifact.weights.size() != ExpectedFeatureNames().size())
    {
        return Refuse(ArtifactRejection::WeightCountMismatch,
            "The artifact has the wrong number of weights for its own feature set.");
    }
    for (const double weight : artifact.weights)
    {
        if (!std::isfinite(weight))
        {
            // A NaN weight makes every score NaN and every comparison false, which
            // produces a confident-looking argmax of zero. Refused rather than scored.
            return Refuse(ArtifactRejection::NonFiniteWeight,
                "The artifact contains a weight that is not a finite number.");
        }
    }
    if (!std::isfinite(artifact.abstainBelow))
    {
        return Refuse(ArtifactRejection::NonFiniteWeight,
            "The artifact's abstention threshold is not a finite number.");
    }

    if (body.contains("qualified_scope") && body["qualified_scope"].is_object())
    {
        const nlohmann::json& scope = body["qualified_scope"];
        if (scope.contains("applications") && scope["applications"].is_array())
        {
            for (const auto& entry : scope["applications"])
            {
                if (entry.is_string())
                {
                    artifact.qualifiedApplications.push_back(
                        Lowered(entry.get<std::string>()));
                }
            }
        }
        if (scope.contains("intents") && scope["intents"].is_array())
        {
            for (const auto& entry : scope["intents"])
            {
                if (entry.is_string())
                {
                    artifact.qualifiedIntents.push_back(Lowered(entry.get<std::string>()));
                }
            }
        }
    }
    if (artifact.qualifiedApplications.empty() || artifact.qualifiedIntents.empty())
    {
        // An artifact that does not say what it was evaluated on cannot be used
        // anywhere, because there is nowhere it has been shown to work.
        return Refuse(ArtifactRejection::NoQualifiedScope,
            "The artifact does not say what it was evaluated on.");
    }

    if (body.contains("held_out") && body["held_out"].is_object())
    {
        const nlohmann::json& held = body["held_out"];
        artifact.heldOutExamples = held.value("examples", 0u);
        artifact.heldOutCoverage = held.value("coverage", 0.0);
        artifact.heldOutAccuracy = held.value("accuracy_on_covered", 0.0);
        artifact.heldOutWrongAndConfident = held.value("wrong_and_confident", 0u);
        artifact.heldOutApplications = held.value("applications", 0u);
        artifact.heldOutTaskFamilies = held.value("task_families", 0u);
        artifact.heldOutLayouts = held.value("layouts", 0u);
        artifact.heldOutSessions = held.value("sessions", 0u);
        artifact.heldOutAbstentions = held.value("abstentions", 0u);
    }
    // The digest covers exactly what decides a ranking: the feature contract, the
    // weights, the threshold and the scope. If those have changed since the artifact
    // was evaluated, its held-out numbers are numbers about a different function.
    const std::string recomputed = Sha256Hex(BehaviourDigestInput(
        artifact.featureVersion, artifact.featureNames, artifact.weights,
        artifact.abstainBelow, artifact.qualifiedApplications, artifact.qualifiedIntents));
    const std::string declared = body.value("behaviour_hash", std::string{});
    if (declared.empty() || declared != recomputed)
    {
        return Refuse(ArtifactRejection::HashMismatch,
            declared.empty()
                ? "The artifact does not carry a behaviour hash, so nothing can confirm "
                  "its weights are the ones it was evaluated with."
                : "The artifact's weights or scope have changed since it was evaluated.");
    }
    artifact.behaviourHash = recomputed;

    if (artifact.heldOutExamples == 0)
    {
        return Refuse(ArtifactRejection::NotQualified,
            "The artifact was never evaluated on data it was not trained on.");
    }
    if (artifact.heldOutWrongAndConfident > 0)
    {
        // One confident mistake on held-out data is one more than a policy that clicks
        // things may have. An abstention costs a model call; this costs a wrong action.
        return Refuse(ArtifactRejection::NotQualified,
            "The artifact made a confident mistake on held-out data.");
    }

    // And how diverse that evidence was.
    //
    // Zero mistakes is necessary and nothing like sufficient. The first artifact this
    // pipeline produced had zero confident mistakes on four held-out examples, from one
    // application, in one layout, in a single session -- and it had learned where things
    // sit on the screen. Each floor below is one way that can happen, and refusing here
    // is the difference between a gate and a note in a document.
    const auto insufficient = [&](const char* axis, const std::uint32_t have,
        const std::uint32_t need)
    {
        return "The artifact is not qualified: " + std::string(axis) + " " +
            std::to_string(have) + ", and a target-selection policy needs at least " +
            std::to_string(need) + ". Zero mistakes on a narrow held-out set is not "
            "evidence that it generalises.";
    };
    if (artifact.heldOutExamples < QualificationFloor::Examples)
    {
        return Refuse(ArtifactRejection::NotQualified,
            insufficient("held-out examples", artifact.heldOutExamples,
                QualificationFloor::Examples));
    }
    if (artifact.heldOutApplications < QualificationFloor::Applications)
    {
        return Refuse(ArtifactRejection::NotQualified,
            insufficient("held-out applications", artifact.heldOutApplications,
                QualificationFloor::Applications));
    }
    if (artifact.heldOutTaskFamilies < QualificationFloor::TaskFamilies)
    {
        return Refuse(ArtifactRejection::NotQualified,
            insufficient("held-out task families", artifact.heldOutTaskFamilies,
                QualificationFloor::TaskFamilies));
    }
    if (artifact.heldOutLayouts < QualificationFloor::Layouts)
    {
        return Refuse(ArtifactRejection::NotQualified,
            insufficient("held-out layouts", artifact.heldOutLayouts,
                QualificationFloor::Layouts));
    }
    if (artifact.heldOutSessions < QualificationFloor::Sessions)
    {
        return Refuse(ArtifactRejection::NotQualified,
            insufficient("held-out sessions", artifact.heldOutSessions,
                QualificationFloor::Sessions));
    }
    if (artifact.heldOutCoverage < QualificationFloor::Coverage)
    {
        return Refuse(ArtifactRejection::NotQualified,
            "The artifact is not qualified: it abstains on more than it decides, so the "
            "model calls it was meant to save are still being made.");
    }


    ArtifactLoad load;
    load.accepted = true;
    load.artifact = std::move(artifact);
    return load;
}

LearnedComputerPolicy::LearnedComputerPolicy(const PayloadVault& payloadVault)
    : vault(&payloadVault)
{
}

ArtifactLoad LearnedComputerPolicy::Load(const std::filesystem::path& path)
{
    ArtifactLoad load = LoadLearnedArtifact(path);
    if (!load.accepted)
    {
        // Left unloaded rather than partly loaded. The mode then reports itself
        // unavailable with this reason, and the routine policy keeps deciding.
        lastRejection = load.detail;
        loaded = LearnedArtifact{};
        return load;
    }
    loaded = load.artifact;
    lastRejection.clear();
    return load;
}

void LearnedComputerPolicy::Unload()
{
    loaded = LearnedArtifact{};
    lastRejection.clear();
}

void LearnedComputerPolicy::SetSubgoal(ComputerSubgoal subgoal)
{
    activeSubgoal = std::move(subgoal);
}

void LearnedComputerPolicy::ClearSubgoal()
{
    activeSubgoal = ComputerSubgoal{};
}

bool LearnedComputerPolicy::WithinQualifiedScope(const ComputerSubgoal& subgoal) const
{
    if (!loaded.Loaded()) return false;
    const std::string application = Lowered(subgoal.target.application);
    const std::string intent = Lowered(ToString(subgoal.intent));
    const bool applicationQualified = std::find(
        loaded.qualifiedApplications.begin(), loaded.qualifiedApplications.end(),
        application) != loaded.qualifiedApplications.end();
    const bool intentQualified = std::find(
        loaded.qualifiedIntents.begin(), loaded.qualifiedIntents.end(),
        intent) != loaded.qualifiedIntents.end();
    return applicationQualified && intentQualified;
}

ComputerDecision LearnedComputerPolicy::Decide(
    const ComputerTaskContext& context, std::stop_token stopToken)
{
    if (stopToken.stop_requested())
    {
        return Answer(ComputerDecisionKind::CannotHandle, ComputerReasonCode::None,
            "The task was stopped.");
    }
    if (!loaded.Loaded())
    {
        return Answer(ComputerDecisionKind::CannotHandle,
            ComputerReasonCode::ProviderUnavailable,
            "No qualified learned artifact is loaded.");
    }
    if (!activeSubgoal.Validated())
    {
        return Answer(ComputerDecisionKind::CannotHandle,
            ComputerReasonCode::OutsideQualifiedScope,
            "This step has no bounded subgoal for a learned decision to work from.");
    }
    if (!WithinQualifiedScope(activeSubgoal))
    {
        // The artifact's own numbers were measured somewhere else. Using it here would
        // be spending a result in a place it was never obtained.
        return Answer(ComputerDecisionKind::CannotHandle,
            ComputerReasonCode::OutsideQualifiedScope,
            "This subgoal is outside the scope this artifact was evaluated on.");
    }
    if (!context.observation.Available() || context.observation.candidates.empty())
    {
        return Answer(ComputerDecisionKind::Reobserve,
            ComputerReasonCode::ObservationUnavailable,
            "There was nothing on screen to rank.");
    }

    const std::size_t count = context.observation.candidates.size();
    std::vector<double> scores;
    scores.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        const std::vector<double> features = CandidateFeatures(
            activeSubgoal, context.observation.candidates[index], index, count);
        double score = 0.0;
        for (std::size_t column = 0; column < features.size(); ++column)
        {
            score += loaded.weights[column] * features[column];
        }
        if (!std::isfinite(score))
        {
            // A score that is not a number cannot be compared, and an argmax over
            // incomparable values is whichever one the loop happened to see first.
            return Answer(ComputerDecisionKind::CannotHandle,
                ComputerReasonCode::MalformedProviderOutput,
                "The ranking produced a value that is not a number.");
        }
        scores.push_back(score);
    }

    const std::vector<double> probabilities = Softmax(scores);
    const std::size_t best = static_cast<std::size_t>(std::distance(
        probabilities.begin(),
        std::max_element(probabilities.begin(), probabilities.end())));

    if (probabilities[best] < loaded.abstainBelow)
    {
        // Abstention is the designed outcome, not a failure. It costs a model call;
        // being confidently wrong costs an action nobody authorized.
        return Answer(ComputerDecisionKind::CannotHandle,
            ComputerReasonCode::OutsideQualifiedScope,
            "No candidate was clear enough to act on.");
    }

    const ObservedCandidate& candidate = context.observation.candidates[best];
    const std::string& application = activeSubgoal.target.application;

    if (activeSubgoal.intent == SubgoalIntent::EnterPayload)
    {
        if (!activeSubgoal.payload.Valid() || vault == nullptr ||
            !vault->Holds(activeSubgoal.payload))
        {
            return Answer(ComputerDecisionKind::CannotHandle,
                ComputerReasonCode::OutsideQualifiedScope,
                "The content this subgoal would enter is not available.");
        }
        ComputerDecision decision = Answer(ComputerDecisionKind::ProposeAction,
            ComputerReasonCode::None,
            "Put the prepared content into \"" + candidate.name + "\".");
        decision.step.description = "Fill \"" + candidate.name + "\" in " + application;
        decision.step.action.type = actions::ActionType::SetControlText;
        decision.step.action.application = application;
        decision.step.action.control = candidate.id;
        decision.step.action.value.clear();
        decision.step.check.type = actions::ActionType::InspectWindow;
        decision.step.check.application = application;
        decision.step.expected = "\"" + candidate.name + "\" (" + candidate.id +
            ") holds the prepared content";
        decision.payload = activeSubgoal.payload;
        return decision;
    }

    ComputerDecision decision = Answer(ComputerDecisionKind::ProposeAction,
        ComputerReasonCode::None, "Use \"" + candidate.name + "\".");
    decision.step.description = "Use \"" + candidate.name + "\" in " + application;
    decision.step.action.type = actions::ActionType::InvokeControl;
    decision.step.action.application = application;
    decision.step.action.control = candidate.id;
    decision.step.check.type = actions::ActionType::InspectWindow;
    decision.step.check.application = application;
    decision.step.expected =
        "\"" + candidate.name + "\" (" + candidate.id + ") was used in " + application;
    return decision;
}

} // namespace revia::computer
