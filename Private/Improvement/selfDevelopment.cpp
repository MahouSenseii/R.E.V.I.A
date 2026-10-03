#include "Improvement/selfDevelopment.h"

#include "Audit/contentDigest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::improvement
{
namespace
{
using json = nlohmann::json;

bool Digest(const std::string& value)
{
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::string(std::istreambuf_iterator<char>(file), {});
}

bool Write(const std::filesystem::path& path, const std::string& content, std::string& error)
{
    static std::atomic<unsigned long> sequence{0};
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        error = "Artifact directory is unavailable.";
        return false;
    }
    const auto pending = path.parent_path() / (path.filename().string() + ".pending-" + std::to_string(++sequence));
#ifdef _WIN32
    HANDLE file = CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = "Exclusive artifact write failed.";
        return false;
    }
    DWORD written = 0;
    const bool complete = content.size() < MAXDWORD &&
                          WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
                          written == content.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!complete || !MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(pending.c_str());
        error = "Atomic artifact publication failed.";
        return false;
    }
#else
    if (std::filesystem::exists(pending, ec))
    {
        error = "Pending artifact already exists.";
        return false;
    }
    {
        std::ofstream file(pending, std::ios::binary);
        file << content;
        if (!file)
        {
            error = "Artifact write failed.";
            return false;
        }
    }
    std::filesystem::rename(pending, path, ec);
    if (ec)
    {
        error = "Atomic artifact publication failed.";
        return false;
    }
#endif
    return true;
}

json Receipt(const ValidationReceipt& value)
{
    return {{"candidate", value.candidateDigest}, {"source", value.sourceDigest}, {"validator", value.validatorDigest},
        {"registry", value.registryDigest}, {"artifact", value.artifactDigest}, {"output", value.outputDigest}};
}

bool PackageMatches(const std::filesystem::path& root, const DevelopmentCandidate& candidate)
{
    return audit::ContentDigest(Read(root / candidate.id / "component.h")) == candidate.candidateDigest;
}

bool ReleaseAbsent(const std::filesystem::path& root)
{
    std::error_code error;
    const bool exists = std::filesystem::exists(root / "active-release.json", error);
    return !error && !exists;
}
}

SelfDevelopment::SelfDevelopment(DevelopmentOptions inputOptions, DevelopmentDependencies inputDependencies)
    : options(std::move(inputOptions)), dependencies(std::move(inputDependencies))
{
}

std::string SelfDevelopment::TargetPath()
{
    return "Desktop/studioDuration.h";
}

std::string SelfDevelopment::Baseline()
{
    return "#pragma once\n\n#include <QString>\n#include <cstdint>\n\nnamespace revia::desktop\n{\n"
           "inline QString StudioDuration(const std::uint64_t milliseconds)\n{\n"
           "    return QString::number(static_cast<double>(milliseconds) / 1000.0, 'f', 2) + \" s\";\n}\n}\n";
}

bool SelfDevelopment::ClosedChange(const CodeChange& change, std::string& content, int& digits, std::string& error)
{
    content.clear();
    if (change.path != TargetPath())
    {
        error = "Target is outside the approved presentation contract.";
        return false;
    }
    const auto changed = ApplyChange(Baseline(), change);
    if (!changed)
    {
        error = "Change does not apply exactly to the approved baseline.";
        return false;
    }
    for (const int allowed : {1, 3})
    {
        std::string expected = Baseline();
        expected.replace(expected.find("'f', 2)"), 7, "'f', " + std::to_string(allowed) + ")");
        if (*changed == expected)
        {
            content = expected;
            digits = allowed;
            return true;
        }
    }
    error = "Only one approved presentation literal may change.";
    return false;
}

std::filesystem::path SelfDevelopment::Target(std::string& error) const
{
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(options.sourceRoot, ec);
    if (ec || options.sourceRoot.empty())
    {
        error = "Source root is unavailable.";
        return {};
    }
    const auto path = std::filesystem::weakly_canonical(root / TargetPath(), ec);
    if (ec || path.lexically_relative(root).generic_string() != TargetPath())
    {
        error = "Target identity changed or escaped its source root.";
        return {};
    }
    return path;
}

bool SelfDevelopment::Admit(const DevelopmentStage stage, const DevelopmentSnapshot& state, std::string& error) const
{
    if (state.incomplete)
    {
        error = "A prior effect has an incomplete durable result; inspect before proceeding.";
        return false;
    }
    if (!options.allowedStages.contains(stage) || !dependencies.admit || options.configuredIdentity.empty() ||
        state.candidate.origin.configuredIdentity != options.configuredIdentity || state.candidate.origin.providerIdentity.empty() ||
        state.candidate.origin.stamp.companionId.empty() || state.candidate.origin.stamp.sessionId.empty() ||
        state.candidate.origin.stamp.generation == 0 || state.candidate.origin.stamp.taskId.empty() ||
        state.candidate.origin.stamp.attemptId.empty())
    {
        error = "Configured provenance and current scoped admission are required.";
        return false;
    }
    try
    {
        error = dependencies.admit(stage, state.candidate);
        return error.empty();
    }
    catch (...)
    {
        error = "Current development admission failed.";
        return false;
    }
}

bool SelfDevelopment::Fresh(const DevelopmentSnapshot& state, const bool integrated, std::string& error) const
{
    try
    {
        if (!dependencies.sourceFingerprint || !dependencies.validatorFingerprint || !dependencies.registryFingerprint)
        {
            error = "Current source and validation identities are unavailable.";
            return false;
        }
        const auto source = dependencies.sourceFingerprint();
        if (!Digest(source) || source != (integrated ? state.integratedSourceDigest : state.candidate.sourceDigest))
        {
            error = "Source changed; fresh validation is required.";
            return false;
        }
        if (state.validation && (state.validation->validatorDigest != dependencies.validatorFingerprint() ||
                                    state.validation->registryDigest != dependencies.registryFingerprint()))
        {
            error = "Validator or registry changed; acceptance is invalid.";
            return false;
        }
        const auto target = Target(error);
        if (target.empty() ||
            audit::ContentDigest(Read(target)) != (integrated ? state.candidate.candidateDigest : state.candidate.baseDigest))
        {
            error = "Target drifted; later work will not be overwritten.";
            return false;
        }
        return true;
    }
    catch (...)
    {
        error = "Current development identities could not be obtained.";
        return false;
    }
}

bool SelfDevelopment::Persist(const DevelopmentSnapshot& state, const std::string& intent, std::string& error) const
{
    if (options.artifactRoot.empty())
    {
        error = "Private artifact root is unavailable.";
        return false;
    }
    const auto& candidate = state.candidate;
    const auto& origin = candidate.origin;
    json record = {{"version", 1}, {"id", candidate.id}, {"path", TargetPath()}, {"intent", intent}, {"base", candidate.baseDigest},
        {"source", candidate.sourceDigest}, {"candidate", candidate.candidateDigest}, {"identity", origin.configuredIdentity},
        {"provider", origin.providerIdentity}, {"fixture", origin.fixture}, {"companion", origin.stamp.companionId},
        {"session", origin.stamp.sessionId}, {"generation", origin.stamp.generation}, {"task", origin.stamp.taskId},
        {"attempt", origin.stamp.attemptId}, {"policy", origin.stamp.policyVersion}, {"accepted", state.accepted},
        {"integrated", state.integrated}, {"packaged", state.packaged}, {"activated", state.activated}, {"recovered", state.recovered},
        {"quarantined", state.quarantined}, {"sourceRecovered", state.sourceRecovered}, {"incomplete", state.incomplete},
        {"integratedSource", state.integratedSourceDigest}};
    if (state.validation)
    {
        record["validation"] = Receipt(*state.validation);
        record["validationDigest"] = state.validation->digest;
    }
    return Write(options.artifactRoot / candidate.id / "receipt.json", record.dump(2), error);
}

bool SelfDevelopment::Store(const DevelopmentSnapshot& state, const std::string& intent, std::string& error)
{
    if (!Persist(state, intent, error))
    {
        std::lock_guard lock(mutex);
        if (state.integrated || state.packaged || state.activated || state.sourceRecovered)
        {
            auto incomplete = state;
            incomplete.incomplete = true;
            candidates[state.candidate.id] = std::move(incomplete);
        }
        else if (!state.accepted)
        {
            const auto previous = candidates.find(state.candidate.id);
            if (previous != candidates.end())
                previous->second.accepted = false;
        }
        return false;
    }
    std::lock_guard lock(mutex);
    candidates[state.candidate.id] = state;
    return true;
}

std::optional<DevelopmentSnapshot> SelfDevelopment::Snapshot(const std::string& id) const
{
    std::lock_guard lock(mutex);
    const auto found = candidates.find(id);
    return found == candidates.end() ? std::nullopt : std::optional(found->second);
}

bool SelfDevelopment::Propose(const CodeChange& change, DevelopmentOrigin origin, DevelopmentCandidate& outCandidate, std::string& error)
{
    std::lock_guard run(execution);
    error.clear();
    DevelopmentSnapshot state;
    auto& candidate = state.candidate;
    candidate.change = change;
    candidate.origin = std::move(origin);
    if (!ClosedChange(change, candidate.content, candidate.digits, error))
        return false;
    candidate.baseDigest = audit::ContentDigest(Baseline());
    candidate.candidateDigest = audit::ContentDigest(candidate.content);
    try
    {
        candidate.sourceDigest = dependencies.sourceFingerprint ? dependencies.sourceFingerprint() : "";
    }
    catch (...)
    {
        error = "Source identity is unavailable.";
        return false;
    }
    candidate.id =
        audit::ContentDigest(candidate.candidateDigest + "\n" + candidate.sourceDigest + "\n" + candidate.origin.stamp.sessionId + "\n" +
                             candidate.origin.stamp.taskId + "\n" + candidate.origin.stamp.attemptId);
    if (std::filesystem::exists(options.artifactRoot / ("quarantine-" + candidate.candidateDigest + ".json")))
    {
        error = "This failed candidate is quarantined.";
        return false;
    }
    if (Snapshot(candidate.id))
    {
        error = "This exact attempt already has a candidate.";
        return false;
    }
    if (!Admit(DevelopmentStage::Propose, state, error) || !Fresh(state, false, error) || !Store(state, "proposed", error))
        return false;
    outCandidate = candidate;
    return true;
}

bool SelfDevelopment::Validate(const std::string& id, const std::stop_token stopToken, std::string& error)
{
    std::lock_guard run(execution);
    auto state = Snapshot(id);
    if (!state || state->integrated || state->quarantined || !dependencies.validate)
    {
        error = "Candidate validation is unavailable.";
        return false;
    }
    if (stopToken.stop_requested() || !Admit(DevelopmentStage::Validate, *state, error) || !Fresh(*state, false, error))
        return false;
    DevelopmentValidation result;
    try
    {
        result = dependencies.validate(state->candidate, stopToken);
    }
    catch (...)
    {
        error = "Trusted candidate validation failed.";
        return false;
    }
    if (stopToken.stop_requested() || !Admit(DevelopmentStage::Validate, *state, error) || !Fresh(*state, false, error))
        return false;
    if (!result.objectivePassed || !CompletePassingBuild(result.build) || result.candidateDigest != state->candidate.candidateDigest ||
        result.sourceDigest != state->candidate.sourceDigest || !Digest(result.artifactDigest) || !Digest(result.validatorDigest) ||
        result.validatorDigest != dependencies.validatorFingerprint() ||
        audit::ContentDigest(result.build.discoveryOutput) != dependencies.registryFingerprint())
    {
        error = "Complete exact candidate and objective evidence is not established.";
        return false;
    }
    ValidationReceipt receipt;
    receipt.candidateDigest = result.candidateDigest;
    receipt.sourceDigest = result.sourceDigest;
    receipt.validatorDigest = result.validatorDigest;
    receipt.registryDigest = audit::ContentDigest(result.build.discoveryOutput);
    receipt.artifactDigest = result.artifactDigest;
    receipt.outputDigest = audit::ContentDigest(result.build.buildLog + "\n" + result.build.testOutput + "\n" + result.objectiveOutput);
    receipt.digest = audit::ContentDigest(Receipt(receipt).dump());
    state->validation = receipt;
    state->accepted = false;
    return Store(*state, "validated", error);
}

bool SelfDevelopment::Review(const std::string& id, const std::stop_token stopToken, std::string& error)
{
    std::lock_guard run(execution);
    auto state = Snapshot(id);
    if (!state || !state->validation || state->integrated || state->quarantined || !dependencies.review)
    {
        error = "Exact validated review is unavailable.";
        return false;
    }
    if (stopToken.stop_requested() || !Admit(DevelopmentStage::Review, *state, error) || !Fresh(*state, false, error))
        return false;
    DevelopmentReview review;
    try
    {
        review = dependencies.review(state->candidate, *state->validation, stopToken);
    }
    catch (...)
    {
        error = "Candidate review did not complete.";
        return false;
    }
    if (stopToken.stop_requested() || !Admit(DevelopmentStage::Review, *state, error) || !Fresh(*state, false, error))
        return false;
    state->accepted = review.accepted && review.candidateDigest == state->candidate.candidateDigest &&
                      review.validationDigest == state->validation->digest;
    if (!Store(*state, state->accepted ? "review-accepted" : "review-refused", error))
        return false;
    if (!state->accepted)
    {
        error = "Reviewer did not accept this exact validated candidate.";
        return false;
    }
    return true;
}

bool SelfDevelopment::Integrate(const std::string& id, std::string& error)
{
    std::lock_guard run(execution);
    auto state = Snapshot(id);
    if (!state || !state->accepted || !state->validation || state->integrated || state->quarantined)
    {
        error = "Exact accepted integration is unavailable.";
        return false;
    }
    if (!Admit(DevelopmentStage::Integrate, *state, error) || !Fresh(*state, false, error) || !Persist(*state, "integration-intent", error))
        return false;
    if (!Admit(DevelopmentStage::Integrate, *state, error) || !Fresh(*state, false, error))
        return false;
    const auto target = Target(error);
    if (target.empty() || !Write(target, state->candidate.content, error))
        return false;
    state->integrated = true;
    try
    {
        state->integratedSourceDigest = dependencies.sourceFingerprint();
    }
    catch (...)
    {
        state->integratedSourceDigest.clear();
    }
    state->incomplete = !Digest(state->integratedSourceDigest);
    // Record the completed effect in memory even if its durable result cannot be saved.
    {
        std::lock_guard lock(mutex);
        candidates[id] = *state;
    }
    if (!Digest(state->integratedSourceDigest) || !Store(*state, "integrated", error))
    {
        error = "Source integration occurred, but its durable result is unavailable; inspect before retry.";
        return false;
    }
    return true;
}

bool SelfDevelopment::Package(const std::string& id, std::string& error)
{
    std::lock_guard run(execution);
    auto state = Snapshot(id);
    if (!state || !state->integrated || !state->accepted || state->quarantined || state->packaged)
    {
        error = "Accepted component packaging is unavailable.";
        return false;
    }
    if (!Admit(DevelopmentStage::Package, *state, error) || !Fresh(*state, true, error) || !Persist(*state, "package-intent", error))
        return false;
    if (!Admit(DevelopmentStage::Package, *state, error) || !Fresh(*state, true, error) ||
        !Write(options.artifactRoot / id / "component.h", state->candidate.content, error))
        return false;
    state->packaged = true;
    return Store(*state, "packaged", error);
}

bool SelfDevelopment::Activate(const std::string& id, std::string& error)
{
    std::lock_guard run(execution);
    auto state = Snapshot(id);
    if (!state || !state->packaged || state->quarantined || state->activated || !dependencies.releaseProbe)
    {
        error = "Disposable component activation is unavailable.";
        return false;
    }
    if (!Admit(DevelopmentStage::Activate, *state, error) || !Fresh(*state, true, error) ||
        !PackageMatches(options.artifactRoot, state->candidate))
    {
        if (error.empty())
            error = "Package content drifted.";
        return false;
    }
    if (!ReleaseAbsent(options.artifactRoot))
    {
        error = "An existing release requires an independently owned handoff.";
        return false;
    }
    DevelopmentValidation probe;
    try
    {
        probe = dependencies.releaseProbe(
            DevelopmentStage::Activate, state->candidate, *state->validation, options.artifactRoot / id / "component.h");
    }
    catch (...)
    {
        error = "Disposable release probe did not complete.";
        return false;
    }
    if (!probe.objectivePassed || !CompletePassingBuild(probe.build) || probe.candidateDigest != state->candidate.candidateDigest ||
        probe.validatorDigest != state->validation->validatorDigest || probe.artifactDigest != state->validation->artifactDigest ||
        audit::ContentDigest(probe.build.discoveryOutput) != state->validation->registryDigest)
    {
        error = "Exact disposable release execution is unproven.";
        return false;
    }
    if (!Persist(*state, "activation-intent", error) || !Admit(DevelopmentStage::Activate, *state, error) || !Fresh(*state, true, error))
        return false;
    if (!PackageMatches(options.artifactRoot, state->candidate) || !ReleaseAbsent(options.artifactRoot))
    {
        error = "Package or release changed during activation; later work will not be overwritten.";
        return false;
    }
    if (!Write(options.artifactRoot / "active-release.json",
            json{{"id", id}, {"component", state->candidate.candidateDigest}, {"knownGood", state->candidate.baseDigest},
                {"kind", "disposable-component"}}
                .dump(),
            error))
        return false;
    state->activated = true;
    {
        std::lock_guard lock(mutex);
        candidates[id] = *state;
    }
    return Store(*state, "activated-disposable-component", error);
}

bool SelfDevelopment::Recover(const std::string& id, std::string& error)
{
    std::lock_guard run(execution);
    auto state = Snapshot(id);
    if (!state || !state->integrated || !state->activated || state->recovered || state->quarantined || !dependencies.releaseProbe)
    {
        error = "Failed release recovery is unavailable.";
        return false;
    }
    if (!Admit(DevelopmentStage::Recover, *state, error) || !Fresh(*state, true, error) ||
        !PackageMatches(options.artifactRoot, state->candidate))
        return false;
    const auto activeContent = Read(options.artifactRoot / "active-release.json");
    try
    {
        const auto active = json::parse(activeContent);
        if (active.at("id") != id || active.at("component") != state->candidate.candidateDigest)
        {
            error = "Active release changed; recovery will not overwrite it.";
            return false;
        }
    }
    catch (...)
    {
        error = "Active release identity is unavailable.";
        return false;
    }
    const auto releaseUnchanged = [&]
    {
        if (Read(options.artifactRoot / "active-release.json") == activeContent && PackageMatches(options.artifactRoot, state->candidate))
            return true;
        error = "Package or active release changed; recovery will not overwrite later work.";
        return false;
    };
    DevelopmentValidation probe;
    try
    {
        probe = dependencies.releaseProbe(
            DevelopmentStage::Recover, state->candidate, *state->validation, options.artifactRoot / id / "component.h");
    }
    catch (...)
    {
        error = "Release regression probe did not complete.";
        return false;
    }
    if (probe.objectivePassed || !CompleteBuildExecution(probe.build) || probe.candidateDigest != state->candidate.candidateDigest ||
        probe.validatorDigest != state->validation->validatorDigest || probe.artifactDigest != state->validation->artifactDigest ||
        audit::ContentDigest(probe.build.discoveryOutput) != state->validation->registryDigest)
    {
        error = "A current exact release regression is required before recovery.";
        return false;
    }
    if (!Persist(*state, "recovery-intent", error) || !Admit(DevelopmentStage::Recover, *state, error) || !Fresh(*state, true, error) ||
        !releaseUnchanged())
        return false;
    const auto target = Target(error);
    if (target.empty() ||
        !Write(options.artifactRoot / ("quarantine-" + state->candidate.candidateDigest + ".json"),
            json{{"candidate", state->candidate.candidateDigest}, {"id", id}}.dump(), error) ||
        !Admit(DevelopmentStage::Recover, *state, error) || !Fresh(*state, true, error) || !releaseUnchanged() ||
        !Write(target, Baseline(), error))
        return false;
    state->sourceRecovered = state->quarantined = true;
    {
        std::lock_guard lock(mutex);
        candidates[id] = *state;
    }
    if (!Admit(DevelopmentStage::Recover, *state, error) || !Fresh(*state, false, error) || !releaseUnchanged())
    {
        state->incomplete = true;
        {
            std::lock_guard lock(mutex);
            candidates[id] = *state;
        }
        error = "Source was restored; release handoff remains incomplete after admission changed.";
        return false;
    }
    if (!Write(options.artifactRoot / "active-release.json",
            json{{"id", "known-good"}, {"component", state->candidate.baseDigest}, {"kind", "disposable-component"}}.dump(), error))
    {
        state->incomplete = true;
        {
            std::lock_guard lock(mutex);
            candidates[id] = *state;
        }
        error = "Source was restored; durable release handoff remains incomplete.";
        return false;
    }
    state->recovered = true;
    return Store(*state, "recovered-and-quarantined", error);
}
} // namespace revia::improvement
