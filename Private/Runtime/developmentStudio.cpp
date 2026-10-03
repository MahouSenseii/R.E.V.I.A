#include "Runtime/reviaSession.h"

#include "Audit/contentDigest.h"
#include "Core/utf8.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <chrono>

namespace revia::runtime
{
namespace
{
std::string ReadDevelopmentFile(const std::filesystem::path& path, const std::size_t limit = 4 * 1024 * 1024)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error || std::filesystem::file_size(path, error) > limit || error)
        return {};
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return input.bad() ? std::string{} : bytes;
}

bool WriteDevelopmentEvidence(const std::filesystem::path& path, const std::string& bytes)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    return output.good() && ReadDevelopmentFile(path) == bytes;
}

std::string DevelopmentSourceDigest(const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const char* domain : {"Public", "Private", "Desktop", "Tests"})
    {
        std::filesystem::recursive_directory_iterator next(root / domain, error), end;
        for (; next != end && !error; next.increment(error))
        {
            const auto& path = next->path();
            if (next->is_symlink(error))
                return {};
            if (next->is_regular_file(error) && (path.extension() == ".h" || path.extension() == ".cpp"))
                files.push_back(path);
        }
        if (error)
            return {};
    }
    for (const char* file : {"CMakeLists.txt", "Tools/Studio/CMakeLists.txt", "Tools/VerifyStudioCandidate.ps1"})
        files.push_back(root / file);
    std::sort(files.begin(), files.end());
    std::string manifest;
    for (const auto& file : files)
    {
        const auto bytes = ReadDevelopmentFile(file);
        if (bytes.empty())
            return {};
        const std::string relative = file.lexically_relative(root).generic_string();
        manifest += std::to_string(relative.size()) + ":" + relative + ":" + audit::ContentDigest(bytes) + "\n";
    }
    return audit::ContentDigest(manifest);
}

std::string DevelopmentValidatorDigest(const std::filesystem::path& root)
{
    std::string material;
    for (const char* file : {"Tools/VerifyStudioCandidate.ps1", "Tools/Studio/CMakeLists.txt", "Tests/Fixture/studioDurationTests.cpp",
             "Private/Improvement/selfDevelopment.cpp"})
    {
        const auto bytes = ReadDevelopmentFile(root / file);
        if (bytes.empty())
            return {};
        material += std::string(file) + ":" + audit::ContentDigest(bytes) + "\n";
    }
    return audit::ContentDigest(material);
}

struct DevelopmentRegistry
{
    std::mutex mutex;
    std::string source;
    std::string validator;
    std::string discovery;
};

std::string DevelopmentScopeRefusal(actions::ActionRuntime& actionRuntime, const policy::CompanionAuthority& authority,
    const std::filesystem::path& source, const RuntimeStamp& stamp)
{
    actions::ActionRequest request;
    request.type = actions::ActionType::ReadTextFile;
    request.source = source / improvement::SelfDevelopment::TargetPath();
    request.requestedBy = "self-development";
    const auto decision = actionRuntime.Evaluate(request);
    if (decision.verdict != actions::PolicyVerdict::Allowed)
        return "Current machine policy does not admit bounded source review.";
    return authority.Evaluate(stamp, request, decision);
}

bool ProjectDevelopment(improvement::ProposalStore& store, const improvement::DevelopmentSnapshot& snapshot, std::string& error)
{
    const auto& candidate = snapshot.candidate;
    const auto recordId = "closed-" + candidate.id;
    auto proposal = store.Find(recordId).value_or(improvement::CodeProposal{});
    const bool ownerDecided =
        proposal.status == improvement::ProposalStatus::Accepted || proposal.status == improvement::ProposalStatus::Rejected;
    proposal.id = recordId;
    proposal.createdAt =
        std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    proposal.trigger = "request";
    proposal.taskId = candidate.origin.stamp.taskId;
    proposal.title = "Bounded Agent Studio timing presentation";
    proposal.problem = "Dense elapsed-time precision makes timing cards harder to scan.";
    proposal.expectedBenefit = "A pure formatter precision change, preserving every other byte.";
    proposal.risks = "Presentation only; no source integration or release authority is granted.";
    proposal.change = candidate.change;
    proposal.fingerprint = improvement::Fingerprint(candidate.change);
    proposal.evidence = "Candidate SHA256 " + candidate.candidateDigest + "; source SHA256 " + candidate.sourceDigest +
                        "; configured companion " + candidate.origin.configuredIdentity + "; provider " + candidate.origin.providerIdentity;
    if (snapshot.validation)
    {
        const auto& receipt = *snapshot.validation;
        if (proposal.status != improvement::ProposalStatus::Accepted && proposal.status != improvement::ProposalStatus::Rejected)
            proposal.status = improvement::ProposalStatus::Verified;
        proposal.verificationSummary = "Host native formatting cases passed; validation SHA256 " + receipt.digest + "; registry SHA256 " +
                                       receipt.registryDigest + "; artifact SHA256 " + receipt.artifactDigest;
        if (!ownerDecided)
            proposal.feedback = snapshot.accepted
                                    ? "Companion review accepted this exact receipt; owner decision and integration remain separate."
                                    : "Exact objective evidence recorded; companion acceptance is not established.";
    }
    else
        proposal.verificationSummary = "Draft only; native validation and exact companion review are pending.";
    return store.Save(proposal, improvement::MakeUnifiedDiff(improvement::SelfDevelopment::Baseline(), candidate.change), error);
}
}

bool ReviaSession::InitializeDevelopmentStudio(std::string& error)
{
    std::lock_guard lock(developmentStudioMutex);
    if (selfDevelopment)
        return true;
    const auto source = improvement::SourceCatalog::LocateSourceRoot(companionPaths.InstallRoot());
    if (!source || ReadDevelopmentFile(*source / improvement::SelfDevelopment::TargetPath()) != improvement::SelfDevelopment::Baseline())
    {
        error = "The approved presentation baseline is unavailable or has changed. No source integration is enabled.";
        return false;
    }
    const auto artifactRoot = companionPaths.Resolve("RuntimeData/Improvement/ClosedPresentation");
    if (improvementStore->Directory().empty() &&
        !improvementStore->Initialize(companionPaths.Resolve("RuntimeData/Improvement/Proposals"), error))
        return false;
    const auto registry = std::make_shared<DevelopmentRegistry>();
    improvement::DevelopmentOptions options;
    options.sourceRoot = *source;
    options.artifactRoot = artifactRoot;
    options.configuredIdentity = companionPaths.Descriptor().id;
    options.allowedStages = {
        improvement::DevelopmentStage::Propose, improvement::DevelopmentStage::Validate, improvement::DevelopmentStage::Review};
    improvement::DevelopmentDependencies dependencies;
    dependencies.sourceFingerprint = [source] { return DevelopmentSourceDigest(*source); };
    dependencies.validatorFingerprint = [source] { return DevelopmentValidatorDigest(*source); };
    dependencies.registryFingerprint = [source, registry]
    {
        const auto currentSource = DevelopmentSourceDigest(*source);
        const auto currentValidator = DevelopmentValidatorDigest(*source);
        std::lock_guard guard(registry->mutex);
        return registry->source == currentSource && registry->validator == currentValidator && !registry->discovery.empty()
                   ? audit::ContentDigest(registry->discovery)
                   : std::string{};
    };
    dependencies.admit = [this, source](improvement::DevelopmentStage, const improvement::DevelopmentCandidate& candidate)
    {
        if (!Admits(candidate.origin.stamp) || candidate.origin.configuredIdentity != companionPaths.Descriptor().id)
            return std::string("The originating companion session is inactive or mismatched.");
        if (!started.load())
            return std::string("Start the companion before requesting model-led development.");
        return DevelopmentScopeRefusal(actionRuntime, *companionAuthority, *source, candidate.origin.stamp);
    };
    dependencies.validate = [source, artifactRoot, registry](const improvement::DevelopmentCandidate& candidate, const std::stop_token stop)
    {
        improvement::DevelopmentValidation result;
        result.candidateDigest = candidate.candidateDigest;
        result.sourceDigest = candidate.sourceDigest;
        result.validatorDigest = DevelopmentValidatorDigest(*source);
        if (stop.stop_requested())
            return result;
        const auto isolated = artifactRoot / candidate.id / "validation-input";
        std::error_code error;
        std::filesystem::create_directories(isolated / "Desktop", error);
        if (error)
            return result;
        const auto header = isolated / "Desktop/studioDuration.h";
        {
            std::ofstream output(header, std::ios::binary | std::ios::trunc);
            output.write(candidate.content.data(), static_cast<std::streamsize>(candidate.content.size()));
            output.flush();
            if (!output.good())
                return result;
        }
        if (ReadDevelopmentFile(header) != candidate.content)
            return result;
        const auto buildRoot = artifactRoot / candidate.id / "validation-build";
        auto runner = improvement::MakeScriptRunner(
            *source / "Tools/VerifyStudioCandidate.ps1", *source / "build/debug", artifactRoot / candidate.id / "validation-logs", 2, 2);
        result.build = runner(isolated, buildRoot, stop);
        const auto binary = ReadDevelopmentFile(buildRoot / "ReviaStudioDuration.exe", 16 * 1024 * 1024);
        if (!binary.empty())
            result.artifactDigest = audit::ContentDigest(binary);
        result.objectivePassed = improvement::CompletePassingBuild(result.build) && ReadDevelopmentFile(header) == candidate.content &&
                                 result.validatorDigest == DevelopmentValidatorDigest(*source) &&
                                 candidate.sourceDigest == DevelopmentSourceDigest(*source);
        result.objectiveOutput = result.build.testOutput;
        result.objectivePassed =
            result.objectivePassed &&
            WriteDevelopmentEvidence(artifactRoot / candidate.id / "validated-discovery.json", result.build.discoveryOutput);
        if (result.objectivePassed)
        {
            std::lock_guard guard(registry->mutex);
            registry->source = candidate.sourceDigest;
            registry->validator = result.validatorDigest;
            registry->discovery = result.build.discoveryOutput;
        }
        return result;
    };
    dependencies.review = [this, artifactRoot](const improvement::DevelopmentCandidate& candidate,
                              const improvement::ValidationReceipt& receipt, const std::stop_token stop)
    {
        improvement::DevelopmentReview review;
        const std::string schema =
            R"({"type":"object","properties":{"accepted":{"type":"boolean"},"candidateDigest":{"type":"string"},"validationDigest":{"type":"string"},"reason":{"type":"string"}},"required":["accepted","candidateDigest","validationDigest","reason"],"additionalProperties":false})";
        const nlohmann::json material = {{"component", candidate.change.path}, {"content", candidate.content}, {"digits", candidate.digits},
            {"candidateDigest", candidate.candidateDigest}, {"validationDigest", receipt.digest},
            {"objectiveEvidence", "Host-controlled native Qt build and six duration cases passed."},
            {"authority", "Review only; source integration, release, commits and deployment require distinct authority."}};
        const auto response = router.ReviewCode(
            "Review this exact closed presentation change. Return accepted only when it reduces "
            "visual clutter while retaining useful timing and supplied native evidence is sufficient. Copy both exact digests. "
            "Do not claim to have run tests or authorize another stage. Return the required JSON.",
            material.dump(), schema, stop);
        if (!response.bSuccess || stop.stop_requested() || !Admits(candidate.origin.stamp))
            return review;
        try
        {
            const auto payload = nlohmann::json::parse(response.response);
            review.accepted = payload.at("accepted").get<bool>();
            review.candidateDigest = payload.at("candidateDigest").get<std::string>();
            review.validationDigest = payload.at("validationDigest").get<std::string>();
            if (!WriteDevelopmentEvidence(artifactRoot / candidate.id / "model-review.json",
                    nlohmann::json{{"response", payload}, {"provider", response.selectedModel}, {"fixture", false}}.dump(2)))
                return improvement::DevelopmentReview{};
        }
        catch (...)
        {
        }
        return review;
    };
    selfDevelopment = std::make_shared<improvement::SelfDevelopment>(std::move(options), std::move(dependencies));
    return true;
}

bool ReviaSession::ProposePresentationChange(std::string& outId, std::string& error, const std::stop_token stop)
{
    std::lock_guard operationLock(operationMutex);
    if (!InitializeDevelopmentStudio(error))
        return false;
    const auto source = improvement::SourceCatalog::LocateSourceRoot(companionPaths.InstallRoot());
    if (!source || stop.stop_requested())
        return false;
    const auto origin =
        sessionIdentity.Stamp("development-" + actions::NewActionId(), actions::NewActionId(), companionAuthority->Revision());
    policy::AuthorityPermissions restriction;
    restriction.operations = {actions::ActionType::ReadTextFile};
    restriction.roots = {*source / improvement::SelfDevelopment::TargetPath()};
    if (!companionAuthority->RegisterTask(origin, {}, std::move(restriction)))
    {
        error = "Current development task scope could not be registered.";
        return false;
    }
    struct TaskScope
    {
        std::shared_ptr<policy::CompanionAuthority> authority;
        RuntimeStamp stamp;
        bool retained = false;
        ~TaskScope()
        {
            if (!retained)
                authority->EndTask(stamp);
        }
    } scope{companionAuthority, origin};
    error = DevelopmentScopeRefusal(actionRuntime, *companionAuthority, *source, origin);
    if (!started.load() || !Admits(origin) || !error.empty())
    {
        if (error.empty())
            error = "Start the current companion before bounded development.";
        return false;
    }
    if (!EnsureLLMAvailable(stop))
    {
        error = "The configured local model is unavailable.";
        return false;
    }
    const std::string schema =
        R"({"type":"object","properties":{"path":{"type":"string"},"find":{"type":"string"},"replace":{"type":"string"},"reason":{"type":"string"}},"required":["path","find","replace","reason"],"additionalProperties":false})";
    const nlohmann::json material = {{"component", improvement::SelfDevelopment::TargetPath()},
        {"source", improvement::SelfDevelopment::Baseline()},
        {"objective", "Make Agent Studio elapsed times easier to scan without clutter."},
        {"allowedEffect", "Change only the formatter precision literal from 2 to 1 or 3; all other source bytes must be preserved."}};
    const auto response = router.ReviewCode(
        "Propose one bounded presentation improvement. Read the source and objective. "
        "Return JSON with the exact path, exact find snippet, replacement snippet and brief reason. "
        "Use the smallest exact replacement that occurs once. No scripts, tools, private data, deployment or permission changes.",
        material.dump(), schema, stop);
    if (!response.bSuccess || stop.stop_requested() || !Admits(origin))
    {
        error = "The current local model did not produce an admissible presentation proposal.";
        return false;
    }
    try
    {
        const auto payload = nlohmann::json::parse(response.response);
        improvement::CodeChange change{
            payload.at("path").get<std::string>(), payload.at("find").get<std::string>(), payload.at("replace").get<std::string>()};
        improvement::DevelopmentOrigin provenance{origin, companionPaths.Descriptor().id,
            response.selectedModel.empty() ? settings.llm.modelName : response.selectedModel, false};
        improvement::DevelopmentCandidate candidate;
        std::shared_ptr<improvement::SelfDevelopment> controller;
        {
            std::lock_guard lock(developmentStudioMutex);
            controller = selfDevelopment;
        }
        if (!controller->Propose(change, std::move(provenance), candidate, error))
            return false;
        const auto snapshot = controller->Snapshot(candidate.id);
        if (!snapshot || !ProjectDevelopment(*improvementStore, *snapshot, error))
            return false;
        auto projected = improvementStore->Find("closed-" + candidate.id);
        if (!projected)
        {
            error = "The proposal projection could not be retained.";
            return false;
        }
        projected->reason = payload.at("reason").get<std::string>().substr(0, 1024);
        if (!improvementStore->Save(*projected, improvement::MakeUnifiedDiff(improvement::SelfDevelopment::Baseline(), change), error))
            return false;
        std::lock_guard lock(developmentStudioMutex);
        if (const auto previous = controller->Snapshot(presentationCandidateId))
            companionAuthority->EndTask(previous->candidate.origin.stamp);
        presentationCandidateId = outId = candidate.id;
        scope.retained = true;
        return true;
    }
    catch (...)
    {
        error = "The local model's proposal did not match the closed change schema.";
        return false;
    }
}

bool ReviaSession::ReviewPresentationChange(const bool validate, std::string& error, const std::stop_token stop)
{
    std::lock_guard operationLock(operationMutex);
    std::shared_ptr<improvement::SelfDevelopment> controller;
    std::string id;
    {
        std::lock_guard lock(developmentStudioMutex);
        controller = selfDevelopment;
        id = presentationCandidateId;
    }
    if (!controller || id.empty())
    {
        error = "First request a bounded presentation proposal.";
        return false;
    }
    const bool completed = validate ? controller->Validate(id, stop, error) : controller->Review(id, stop, error);
    const auto snapshot = controller->Snapshot(id);
    if (snapshot && !ProjectDevelopment(*improvementStore, *snapshot, error))
        return false;
    return completed;
}

std::optional<improvement::DevelopmentSnapshot> ReviaSession::DevelopmentStudio() const
{
    std::lock_guard lock(developmentStudioMutex);
    return selfDevelopment ? selfDevelopment->Snapshot(presentationCandidateId) : std::nullopt;
}
}
