#pragma once

#include "Improvement/codeProposal.h"
#include "Improvement/workbench.h"
#include "Runtime/runtimeStamp.h"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>

namespace revia::improvement
{
enum class DevelopmentStage
{
    Propose,
    Validate,
    Review,
    Integrate,
    Package,
    Activate,
    Recover
};

struct DevelopmentOrigin
{
    runtime::RuntimeStamp stamp;
    std::string configuredIdentity;
    std::string providerIdentity;
    bool fixture = false;
};

struct DevelopmentCandidate
{
    std::string id;
    CodeChange change;
    DevelopmentOrigin origin;
    std::string baseDigest;
    std::string sourceDigest;
    std::string candidateDigest;
    std::string content;
    int digits = 2;
};

struct DevelopmentValidation
{
    std::string candidateDigest;
    std::string sourceDigest;
    std::string validatorDigest;
    std::string artifactDigest;
    bool objectivePassed = false;
    std::string objectiveOutput;
    BuildOutcome build;
};

struct ValidationReceipt
{
    std::string digest;
    std::string candidateDigest;
    std::string sourceDigest;
    std::string validatorDigest;
    std::string registryDigest;
    std::string artifactDigest;
    std::string outputDigest;
};

struct DevelopmentReview
{
    bool accepted = false;
    std::string candidateDigest;
    std::string validationDigest;
};

struct DevelopmentSnapshot
{
    DevelopmentCandidate candidate;
    std::optional<ValidationReceipt> validation;
    bool accepted = false;
    bool integrated = false;
    bool packaged = false;
    bool activated = false;
    bool recovered = false;
    bool sourceRecovered = false;
    bool incomplete = false;
    bool quarantined = false;
    std::string integratedSourceDigest;
};

struct DevelopmentOptions
{
    std::filesystem::path sourceRoot;
    std::filesystem::path artifactRoot;
    std::string configuredIdentity;
    std::set<DevelopmentStage> allowedStages;
};

struct DevelopmentDependencies
{
    using Fingerprint = std::function<std::string()>;
    using Admission = std::function<std::string(DevelopmentStage, const DevelopmentCandidate&)>;
    using Validator = std::function<DevelopmentValidation(const DevelopmentCandidate&, std::stop_token)>;
    using Reviewer = std::function<DevelopmentReview(const DevelopmentCandidate&, const ValidationReceipt&, std::stop_token)>;
    using ReleaseProbe = std::function<DevelopmentValidation(
        DevelopmentStage, const DevelopmentCandidate&, const ValidationReceipt&, const std::filesystem::path&)>;
    Fingerprint sourceFingerprint;
    Fingerprint validatorFingerprint;
    Fingerprint registryFingerprint;
    Admission admit;
    Validator validate;
    Reviewer review;
    // Actual disposable consumer probe; recovery requires a measured current regression.
    ReleaseProbe releaseProbe;
};

// Closed presentation change only. Host construction owns collaborators; imported records confer no trust.
class SelfDevelopment
{
  public:
    SelfDevelopment(DevelopmentOptions options, DevelopmentDependencies dependencies);
    bool Propose(const CodeChange& change, DevelopmentOrigin origin, DevelopmentCandidate& outCandidate, std::string& error);
    bool Validate(const std::string& id, std::stop_token stopToken, std::string& error);
    bool Review(const std::string& id, std::stop_token stopToken, std::string& error);
    bool Integrate(const std::string& id, std::string& error);
    bool Package(const std::string& id, std::string& error);
    // Selects a disposable local package; never replaces a running executable.
    bool Activate(const std::string& id, std::string& error);
    bool Recover(const std::string& id, std::string& error);
    [[nodiscard]] std::optional<DevelopmentSnapshot> Snapshot(const std::string& id) const;
    [[nodiscard]] static bool ClosedChange(const CodeChange& change, std::string& content, int& digits, std::string& error);
    [[nodiscard]] static std::string Baseline();
    [[nodiscard]] static std::string TargetPath();

  private:
    bool Admit(DevelopmentStage stage, const DevelopmentSnapshot& state, std::string& error) const;
    bool Fresh(const DevelopmentSnapshot& state, bool integrated, std::string& error) const;
    bool Persist(const DevelopmentSnapshot& state, const std::string& intent, std::string& error) const;
    [[nodiscard]] std::filesystem::path Target(std::string& error) const;
    bool Store(const DevelopmentSnapshot& state, const std::string& intent, std::string& error);
    DevelopmentOptions options;
    DevelopmentDependencies dependencies;
    mutable std::mutex mutex;
    std::mutex execution;
    std::map<std::string, DevelopmentSnapshot> candidates;
};
} // namespace revia::improvement
