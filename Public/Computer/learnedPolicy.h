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

// Rejects artifacts with unsupported features, unqualified scope, or mismatched hashes.
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
    // Each diversity axis has its own evidence floor, separate from example count.
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

// Enforced at load: minimum independent applications, task families, layouts,
// sessions, and examples. Qualification permits comparison, not claims of superiority.
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

// Must match Tools/Computer/features.py exactly; exposed for parity checks.
[[nodiscard]] std::vector<double> CandidateFeatures(const ComputerSubgoal& subgoal,
    const ObservedCandidate& candidate, std::size_t index, std::size_t candidateCount);

// Tests use the loader's digest so other rejection paths can be exercised.
[[nodiscard]] std::string BehaviourDigestForTest(std::uint32_t featureVersion, const std::vector<std::string>& featureNames,
    const std::vector<double>& weights, double abstainBelow, const std::vector<std::string>& applications,
    const std::vector<std::string>& intents);

// Ranks one observation without payload access. Missing or unqualified artifacts
// report unavailable so routing can fall back to a supported policy.
class LearnedComputerPolicy final : public IComputerPolicy
{
public:
    explicit LearnedComputerPolicy(const PayloadVault& payloadVault);

    [[nodiscard]] std::string Name() const override { return "learned"; }
    [[nodiscard]] bool IsAvailable() const override { return loaded.Loaded(); }

    [[nodiscard]] ComputerDecision Decide(const ComputerTaskContext& context, std::stop_token stopToken) override;

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
