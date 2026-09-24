#pragma once

#include "Computer/computerPolicy.h"
#include "Computer/computerSubgoal.h"
#include "Computer/payloadVault.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace revia::computer
{

// What an artifact has to prove about itself before it is allowed to decide anything.
//
// Every field here is a way a trained artifact goes wrong quietly. A feature version it
// was fitted under that this build does not implement means the weights would be applied
// to the wrong columns -- and the result would not look like an error, it would look
// like a policy with opinions. A scope it was never evaluated on means a number measured
// in one application being spent in another. A hash that does not match its own contents
// means something edited it.
enum class ArtifactRejection
{
    None,
    Missing,
    Malformed,
    UnsupportedArtifactVersion,
    UnsupportedFeatureVersion,
    WeightCountMismatch,
    FeatureNamesMismatch,
    HashMismatch,
    NonFiniteWeight,
    NoQualifiedScope,
    // Loaded and well-formed, and its own held-out numbers do not support using it.
    // Kept separate from the malformed cases: this one is a judgement about evidence,
    // not about the file.
    NotQualified
};

[[nodiscard]] std::string ToString(ArtifactRejection value);

// An artifact, as the runtime sees it.
struct LearnedArtifact
{
    std::uint32_t artifactVersion = 0;
    std::uint32_t featureVersion = 0;
    std::string hash;
    // Over the weights, threshold, feature contract and scope -- the parts that decide
    // a ranking. Recomputed at load and compared, so an artifact edited after it was
    // evaluated is refused rather than trusted on the strength of its own numbers.
    std::string behaviourHash;
    std::string datasetLineage;
    std::string trainedAt;
    std::vector<std::string> featureNames;
    std::vector<double> weights;
    double abstainBelow = 1.0;
    // What it was evaluated on, and therefore all it may be used for.
    std::vector<std::string> qualifiedApplications;
    std::vector<std::string> qualifiedIntents;
    // Its own held-out numbers, carried so the diagnostics panel can show what the
    // thing is actually claiming rather than only that it loaded.
    std::uint32_t heldOutExamples = 0;
    double heldOutCoverage = 0.0;
    double heldOutAccuracy = 0.0;
    std::uint32_t heldOutWrongAndConfident = 0;
    // How *diverse* the evidence was, kept apart from how much of it there was.
    //
    // Zero confident mistakes on four held-out examples from one application in one
    // layout is not evidence that a policy generalises; it is evidence that four things
    // went well. These are the axes along which a target-selection policy can be wrong
    // in ways a bigger sample from the same source would never reveal, so each is
    // counted separately and each has its own floor.
    std::uint32_t heldOutApplications = 0;
    std::uint32_t heldOutTaskFamilies = 0;
    std::uint32_t heldOutLayouts = 0;
    std::uint32_t heldOutSessions = 0;
    std::uint32_t heldOutAbstentions = 0;

    [[nodiscard]] bool Loaded() const { return !weights.empty(); }
};

struct ArtifactLoad
{
    bool accepted = false;
    ArtifactRejection rejection = ArtifactRejection::None;
    std::string detail;
    LearnedArtifact artifact;
};

// Load and check one artifact. Pure with respect to everything but the filesystem, so
// every refusal above is testable without training anything.
// The minimum evidence before a target-selection policy may be called qualified.
//
// Written down as numbers, in one place, and enforced at load rather than described in a
// document nobody reads at the moment it matters. Each floor exists because of a way a
// policy can look good without being good:
//
//   applications  one application's controls are one vendor's conventions. A ranker that
//                 has only ever seen the fixture has learned the fixture.
//   task families a policy evaluated only on "press a named button" says nothing about
//                 placing content in an unnamed field.
//   layouts       the first trained artifact put +5.59 on position. A held-out set that
//                 never moves anything cannot catch that.
//   sessions      adjacent steps in one session share a screen; splitting inside one
//                 measures memorisation.
//   examples      the number everything else is conditional on. Zero mistakes out of
//                 four is not a rate.
//
// These are floors, not targets. Meeting them makes an artifact eligible to be compared
// against the deterministic policy; it does not make it better than one.
struct QualificationFloor
{
    static constexpr std::uint32_t Applications = 2;
    static constexpr std::uint32_t TaskFamilies = 3;
    static constexpr std::uint32_t Layouts = 2;
    static constexpr std::uint32_t Sessions = 3;
    static constexpr std::uint32_t Examples = 50;
    // Coverage below this means the artifact abstains so often that the calls it was
    // meant to save are still being made.
    static constexpr double Coverage = 0.40;
};

[[nodiscard]] ArtifactLoad LoadLearnedArtifact(const std::filesystem::path& path);

// The features the artifact was trained on, computed here exactly as Tools/Computer/
// features.py computes them.
//
// Exposed because the parity check needs to call it directly. Two implementations of one
// feature vector is a thing that drifts, and the only defence is a test that computes
// both and compares -- which needs this to be reachable from outside the policy.
[[nodiscard]] std::vector<double> CandidateFeatures(
    const ComputerSubgoal& subgoal,
    const ObservedCandidate& candidate,
    std::size_t index,
    std::size_t candidateCount);

// The behaviour digest an artifact carries, computed the way the loader computes it.
//
// Exposed for the tests that build an artifact and need a hash that matches it. Without
// this every "a bad artifact is refused" test would be refused for the hash rather than
// for the reason it was written to check, and would pass while proving nothing.
[[nodiscard]] std::string BehaviourDigestForTest(
    std::uint32_t featureVersion,
    const std::vector<std::string>& featureNames,
    const std::vector<double>& weights,
    double abstainBelow,
    const std::vector<std::string>& applications,
    const std::vector<std::string>& intents);

// The optional learned backend.
//
// It ranks the candidates of one observation and either names one or declines. It is
// not a language model, it generates nothing, and it holds no payload -- the same
// arrangement the routine policy has, for the same reasons.
//
// Without a qualified artifact it reports itself unavailable and is never asked. That is
// the whole of its failure behaviour: a missing file, an unreadable file, a file from a
// future schema, a file whose numbers do not support using it -- all of them end here,
// with the mode falling back to the policy that works and the reason available to be
// read. None of them end with a randomly initialised model deciding what to click.
class LearnedComputerPolicy final : public IComputerPolicy
{
public:
    explicit LearnedComputerPolicy(const PayloadVault& payloadVault);

    [[nodiscard]] std::string Name() const override { return "learned"; }
    [[nodiscard]] bool IsAvailable() const override { return loaded.Loaded(); }

    [[nodiscard]] ComputerDecision Decide(
        const ComputerTaskContext& context,
        std::stop_token stopToken) override;

    // Install an artifact, or report why not. Switching artifacts is a task-boundary
    // operation; nothing here is safe to do mid-run and nothing calls it mid-run.
    [[nodiscard]] ArtifactLoad Load(const std::filesystem::path& path);
    void Unload();

    [[nodiscard]] const LearnedArtifact& Artifact() const { return loaded; }
    [[nodiscard]] const std::string& LastRejection() const { return lastRejection; }

    // The subgoal it is working toward, for the same reason the routine policy has one.
    void SetSubgoal(ComputerSubgoal subgoal);
    void ClearSubgoal();

    // Whether this artifact was evaluated on the thing it is about to be used for.
    [[nodiscard]] bool WithinQualifiedScope(const ComputerSubgoal& subgoal) const;

private:
    const PayloadVault* vault = nullptr;
    LearnedArtifact loaded;
    std::string lastRejection;
    ComputerSubgoal activeSubgoal;
};

} // namespace revia::computer
