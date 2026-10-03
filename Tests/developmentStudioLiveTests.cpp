#include "Runtime/reviaSession.h"

#include "Audit/contentDigest.h"
#include "Core/utf8.h"
#include "Improvement/selfDevelopment.h"
#include "Policy/capabilityPolicy.h"
#include "Policy/companionAuthority.h"
#include "Runtime/runtimeStamp.h"
#include "testSupport.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::runtime
{
struct ReviaSessionTestAccess
{
    static void ConfigureDevelopment(
        ReviaSession& session, const std::string& model, const int port, const std::filesystem::path& capabilities)
    {
        session.settings.llm.backend = "LLamaCpp";
        session.settings.llm.host = "127.0.0.1";
        session.settings.llm.port = port;
        session.settings.llm.modelName = model;
        session.settings.llm.bAutoStartServer = session.settings.llm.bShutdownServerOnExit = false;
        session.settings.llm.bVisionEnabled = session.settings.llm.bAllowPromptCache = false;
        session.settings.llm.temperature = 0.2F;
        session.settings.llm.maxTokens = 768;
        session.settings.llm.bAutoMaxTokens = false;
        embeddingSettings embedding;
        embedding.bEnabled = embedding.bAutoStartServer = false;
        aiProfile profile;
        profile.bMemoryEnabled = false;
        session.router.ApplyLLMSettings(session.settings.llm, embedding, profile);
        std::string error;
        revia::tests::Check(
            session.actionRuntime.Initialize(capabilities, session.Paths().Resolve("RuntimeData/Logs/development-actions.jsonl"), error),
            error);
        session.started.store(true);
    }

    static void Quiet(ReviaSession& session)
    {
        session.started.store(false);
    }
    static std::vector<improvement::CodeProposal> Proposals(const ReviaSession& session)
    {
        return session.improvementStore->All();
    }
    static responseOutput Review(
        ReviaSession& session, const std::string& instructions, const std::string& material, const std::string& schema)
    {
        return session.router.ReviewCode(instructions, material, schema);
    }
};
}

namespace
{
using namespace revia;
using namespace revia::improvement;
using revia::tests::Check;
using json = nlohmann::json;

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Check(input.good(), "Required controlled evidence is unavailable: " + actions::PathToUtf8(path));
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void Write(const std::filesystem::path& path, const std::string& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.flush();
    Check(output.good(), "Controlled evidence could not be retained.");
}

std::string Validator(const std::filesystem::path& source)
{
    std::string material;
    for (const auto* path : {"Tools/VerifyStudioCandidate.ps1", "Tools/Studio/CMakeLists.txt", "Tests/Fixture/studioDurationTests.cpp",
             "Private/Improvement/selfDevelopment.cpp"})
        material += std::string(path) + ":" + audit::ContentDigest(Read(source / path)) + "\n";
    return audit::ContentDigest(material);
}

std::string Source(const std::filesystem::path& source)
{
    std::vector<std::filesystem::path> files;
    for (const auto* domain : {"Public", "Private", "Desktop", "Tests"})
        for (const auto& item : std::filesystem::recursive_directory_iterator(source / domain))
        {
            Check(!item.is_symlink(), "Controlled source crosses a link.");
            if (item.is_regular_file() && (item.path().extension() == ".h" || item.path().extension() == ".cpp"))
                files.push_back(item.path());
        }
    for (const auto* path : {"CMakeLists.txt", "Tools/Studio/CMakeLists.txt", "Tools/VerifyStudioCandidate.ps1"})
        files.push_back(source / path);
    std::sort(files.begin(), files.end());
    std::string material;
    for (const auto& path : files)
    {
        const auto relative = path.lexically_relative(source).generic_string();
        material += std::to_string(relative.size()) + ":" + relative + ":" + audit::ContentDigest(Read(path)) + "\n";
    }
    return audit::ContentDigest(material);
}

void CopyControlledSource(const std::filesystem::path& source, const std::filesystem::path& target)
{
    for (const auto* domain : {"Public", "Private", "Desktop", "Tests"})
        std::filesystem::create_directories(target / domain);
    for (const auto* path :
        {"CMakeLists.txt", "Desktop/studioDuration.h", "Public/Runtime/runtimeStamp.h", "Private/Improvement/selfDevelopment.cpp",
            "Tools/VerifyStudioCandidate.ps1", "Tools/Studio/CMakeLists.txt", "Tests/Fixture/studioDurationTests.cpp"})
        Write(target / path, Read(source / path));
    Check(Read(target / SelfDevelopment::TargetPath()) == SelfDevelopment::Baseline(), "Controlled baseline drifted.");
}

void TestProductionMissingScopeRefusesBeforeModel()
{
    tests::ScopedTestDirectory fixture;
    Write(fixture.root / SelfDevelopment::TargetPath(), SelfDevelopment::Baseline());
    Write(fixture.root / "CMakeLists.txt", "# controlled fixture\n");
    std::filesystem::create_directories(fixture.root / "Public");
    std::filesystem::create_directories(fixture.root / "Private");
    runtime::ReviaSession session(runtime::CompanionPaths(fixture.root, {"development-default", "Controlled fixture", "assistant", false}));
    std::string id, error;
    Check(!session.ProposePresentationChange(id, error) && !session.DevelopmentStudio() &&
              Read(fixture.root / SelfDevelopment::TargetPath()) == SelfDevelopment::Baseline(),
        "A stopped/uninitialized production session invoked development or changed source.");
    std::cout << "PASS production missing current scope refuses before model exposure or source effects\n";
}

runtime::RuntimeStamp DevelopmentSessionScope(runtime::RuntimeStamp stamp)
{
    stamp.taskId.clear();
    stamp.attemptId.clear();
    return stamp;
}

void TestControlledSessionAndTaskRegistration()
{
    tests::ScopedTestDirectory fixture;
    const auto target = fixture.root / "approved.h";
    std::ofstream(target) << "controlled registration fixture\n";
    actions::CapabilitySettings ceiling;
    ceiling.mode = actions::ExecutionMode::ApprovedScope;
    ceiling.approvedRoots = {target};
    policy::CapabilityPolicy machine(ceiling);
    policy::CompanionAuthority authority;
    runtime::RuntimeStamp task{"controlled-companion", "controlled-session", 7, "exact-task", "exact-attempt", 13};
    authority.SetCompanionDefaults(task.companionId, policy::AuthorityPermissions::WithinMachineCeiling());
    Check(!authority.RegisterSession(task), "Production authority accepted a task stamp as a session registration.");
    const auto session = DevelopmentSessionScope(task);
    Check(authority.RegisterSession(session), "Controlled live session scope was not registered.");
    Check(session.SameSession(task) && session.taskId.empty() && session.attemptId.empty() && session.policyVersion == task.policyVersion &&
              task.taskId == "exact-task" && task.attemptId == "exact-attempt",
        "Session registration changed task provenance or retained task/attempt scope.");
    actions::ActionRequest read;
    read.type = actions::ActionType::ReadTextFile;
    read.source = target;
    Check(!authority.Evaluate(task, read, machine.Evaluate(read)).empty(), "An unregistered task acquired session authority.");
    policy::AuthorityPermissions restriction;
    restriction.operations = {actions::ActionType::ReadTextFile};
    restriction.roots = {target};
    Check(authority.RegisterTask(task, {}, restriction), "Controlled live task scope was not registered.");
    Check(authority.Evaluate(task, read, machine.Evaluate(read)).empty(), "Exact registered task cannot read its approved target.");
    auto unrelated = task;
    unrelated.taskId = "other-task";
    Check(!authority.Evaluate(unrelated, read, machine.Evaluate(read)).empty(), "Registration admitted an unrelated task.");
    auto write = read;
    write.type = actions::ActionType::RenamePath;
    write.destination = target;
    Check(!authority.Evaluate(task, write, machine.Evaluate(write)).empty(), "Read-only task gained write authority.");
    read.source = fixture.root / "outside.h";
    Check(!authority.Evaluate(task, read, machine.Evaluate(read)).empty(), "Registration bypassed the machine resource ceiling.");
    authority.EndSession(session);
    read.source = target;
    Check(!authority.Evaluate(task, read, machine.Evaluate(read)).empty(), "Retired session retained task authority.");
    std::cout << "PASS controlled session/task split retains exact provenance and live authority restrictions\n";
}

struct ProcessResult
{
    int exit = -1;
    std::string output;
};

ProcessResult Run(const std::wstring& application, const std::wstring& arguments, const std::filesystem::path& cwd)
{
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    Check(CreatePipe(&read, &write, &security, 0) != FALSE, "Controlled native pipe creation failed.");
    Check(SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0) != FALSE, "Controlled pipe inheritance failed.");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = write;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        if (job)
            CloseHandle(job);
        CloseHandle(read);
        CloseHandle(write);
        throw std::runtime_error("Controlled native lifetime containment failed.");
    }
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + application + L"\" " + arguments;
    const bool created = CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, cwd.c_str(), &startup, &process) != FALSE;
    CloseHandle(write);
    if (!created)
    {
        CloseHandle(job);
        CloseHandle(read);
        throw std::runtime_error("Controlled native process launch failed.");
    }
    if (!AssignProcessToJobObject(job, process.hProcess) || ResumeThread(process.hThread) == static_cast<DWORD>(-1))
    {
        TerminateProcess(process.hProcess, 125);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        CloseHandle(read);
        throw std::runtime_error("Controlled native child lifetime could not be bound.");
    }
    CloseHandle(process.hThread);
    ProcessResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;)
    {
        DWORD available = 0;
        if (PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) && available)
        {
            char bytes[4096];
            DWORD count = 0;
            if (ReadFile(read, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr))
                result.output.append(bytes, count);
        }
        if (WaitForSingleObject(process.hProcess, 10) == WAIT_OBJECT_0)
            break;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            TerminateProcess(process.hProcess, 124);
            WaitForSingleObject(process.hProcess, 5000);
            break;
        }
    }
    CloseHandle(job);
    for (;;)
    {
        char bytes[4096];
        DWORD count = 0;
        if (!ReadFile(read, bytes, sizeof(bytes), &count, nullptr) || !count)
            break;
        result.output.append(bytes, count);
    }
    DWORD exit = 1;
    if (GetExitCodeProcess(process.hProcess, &exit))
        result.exit = static_cast<int>(exit);
    CloseHandle(read);
    CloseHandle(process.hProcess);
    return result;
#else
    throw std::runtime_error("The controlled native live harness currently requires Windows.");
#endif
}

class LocalLifecycle
{
  public:
    LocalLifecycle(runtime::ReviaSession& modelSession, std::filesystem::path trustedSource, std::filesystem::path targetSource,
        std::filesystem::path evidence, const DevelopmentOrigin& origin, const bool disposable)
        : session(modelSession), trusted(std::move(trustedSource)), source(std::move(targetSource)), artifacts(std::move(evidence))
    {
        DevelopmentOptions options{source, artifacts, origin.configuredIdentity,
            {DevelopmentStage::Propose, DevelopmentStage::Validate, DevelopmentStage::Review, DevelopmentStage::Integrate}};
        if (disposable)
            options.allowedStages.insert({DevelopmentStage::Package, DevelopmentStage::Activate, DevelopmentStage::Recover});
        auto ceiling = actions::CapabilitySettings{};
        ceiling.mode = actions::ExecutionMode::ApprovedScope;
        ceiling.approvedRoots = {source / SelfDevelopment::TargetPath()};
        machine = std::make_unique<policy::CapabilityPolicy>(ceiling);
        authority.SetCompanionDefaults(origin.stamp.companionId, policy::AuthorityPermissions::WithinMachineCeiling());
        Check(authority.RegisterSession(DevelopmentSessionScope(origin.stamp)), "Controlled live session scope was not registered.");
        policy::AuthorityPermissions restriction;
        restriction.operations = {actions::ActionType::ReadTextFile};
        restriction.roots = {source / SelfDevelopment::TargetPath()};
        Check(authority.RegisterTask(origin.stamp, {}, restriction), "Controlled live task scope was not registered.");
        DevelopmentDependencies dependencies;
        dependencies.sourceFingerprint = [this] { return Source(source); };
        dependencies.validatorFingerprint = [this] { return Validator(trusted); };
        dependencies.registryFingerprint = [this] { return registry; };
        dependencies.admit = [this](auto, const auto& candidate)
        {
            if (!session.Admits(candidate.origin.stamp))
                return std::string("Originating local-model session retired.");
            actions::ActionRequest request;
            request.type = actions::ActionType::ReadTextFile;
            request.source = source / SelfDevelopment::TargetPath();
            return authority.Evaluate(candidate.origin.stamp, request, machine->Evaluate(request));
        };
        dependencies.validate = [this](const auto& candidate, auto stop)
        {
            const auto input = artifacts / candidate.id / "input";
            Write(input / SelfDevelopment::TargetPath(), candidate.content);
            build = artifacts / candidate.id / "build";
            auto runner = MakeScriptRunner(
                trusted / "Tools/VerifyStudioCandidate.ps1", trusted / "build/debug", artifacts / candidate.id / "logs", 2, 2);
            validation = {};
            validation.candidateDigest = candidate.candidateDigest;
            validation.sourceDigest = candidate.sourceDigest;
            validation.validatorDigest = Validator(trusted);
            validation.build = runner(input, build, stop);
            validation.objectivePassed =
                CompletePassingBuild(validation.build) && Read(input / SelfDevelopment::TargetPath()) == candidate.content;
            validation.objectiveOutput = validation.build.testOutput;
            if (validation.objectivePassed)
            {
                registry = audit::ContentDigest(validation.build.discoveryOutput);
                validation.artifactDigest = audit::ContentDigest(Read(build / "ReviaStudioDuration.exe"));
                Write(artifacts / candidate.id / "actual-discovery.json", validation.build.discoveryOutput);
            }
            return validation;
        };
        dependencies.review = [this](const auto& candidate, const auto& receipt, auto)
        {
            const std::string schema =
                R"({"type":"object","properties":{"accepted":{"type":"boolean"},"candidateDigest":{"type":"string"},"validationDigest":{"type":"string"},"reason":{"type":"string"}},"required":["accepted","candidateDigest","validationDigest","reason"],"additionalProperties":false})";
            const json material = {{"content", candidate.content}, {"candidateDigest", candidate.candidateDigest},
                {"validationDigest", receipt.digest}, {"objectiveEvidence", validation.objectiveOutput}};
            const auto response = runtime::ReviaSessionTestAccess::Review(session,
                "Review this exact one-literal presentation improvement. Accept only useful reduced clutter with sufficient supplied "
                "host native evidence. Copy both exact digests. Do not authorize another stage or claim tests you did not run.",
                material.dump(), schema);
            Check(response.bSuccess, "Actual local-model review did not complete.");
            const auto payload = json::parse(response.response);
            Write(artifacts / candidate.id / "actual-model-review.json",
                json{{"response", payload}, {"configuredProvider", response.selectedModel}, {"fixture", false}}.dump(2));
            return DevelopmentReview{payload.at("accepted").template get<bool>(), payload.at("candidateDigest").template get<std::string>(),
                payload.at("validationDigest").template get<std::string>()};
        };
        dependencies.releaseProbe = [this](auto stage, const auto& candidate, const auto&, const auto& package)
        {
            Check(Read(package) == candidate.content, "Actual disposable package drifted before its consumer probe.");
            auto current = validation;
            const auto discovery = Run(ctest, L"--show-only=json-v1", build);
            const auto suite = Run(ctest, L"--output-on-failure", build);
            current.build.discoveryExitCode = discovery.exit;
            current.build.testExitCode = suite.exit;
            current.build.discoveryOutput = discovery.output;
            current.build.testOutput = suite.output;
            current.artifactDigest = audit::ContentDigest(Read(build / "ReviaStudioDuration.exe"));
            // Deliberate host fault injection: a consumer asks the accepted artifact for baseline precision.
            const auto objective = Run((build / "ReviaStudioDuration.exe").wstring(),
                std::to_wstring(stage == DevelopmentStage::Recover ? 2 : candidate.digits), build);
            current.objectivePassed = objective.exit == 0;
            current.objectiveOutput = objective.output;
            Write(artifacts / candidate.id / (stage == DevelopmentStage::Recover ? "injected-consumer-fault.txt" : "release-consumer.txt"),
                "actualExit=" + std::to_string(objective.exit) + "\n" + objective.output + "\n" + suite.output);
            return current;
        };
        owner = std::make_unique<SelfDevelopment>(std::move(options), std::move(dependencies));
    }

    std::unique_ptr<SelfDevelopment> owner;
    std::wstring ctest;

  private:
    runtime::ReviaSession& session;
    std::filesystem::path trusted, source, artifacts, build;
    DevelopmentValidation validation;
    std::string registry;
    policy::CompanionAuthority authority;
    std::unique_ptr<policy::CapabilityPolicy> machine;
};

void RunLive(const std::filesystem::path& source, const std::filesystem::path& evidence, const std::string& model, const int port,
    const std::wstring& ctest, const bool integrateReal)
{
    Check(source.is_absolute() && evidence.is_absolute() && !model.empty(), "Explicit source/evidence/model identity is required.");
    Check(!std::filesystem::exists(evidence), "Live evidence root must be a fresh dedicated directory.");
    const auto controlled = evidence / "controlled-source";
    CopyControlledSource(source, controlled);
    Write(controlled / "capabilities.json",
        json{{"mode", "approved_scope"}, {"approvedRoots", {actions::PathToUtf8(controlled)}}, {"autoApproveRiskThrough", "read_only"}}
            .dump());
    runtime::ReviaSession session(
        runtime::CompanionPaths(controlled, {"development-live", "Actual bounded development", "assistant", false}));
    runtime::ReviaSessionTestAccess::ConfigureDevelopment(session, model, port, controlled / "capabilities.json");
    struct QuietSession
    {
        runtime::ReviaSession& session;
        ~QuietSession()
        {
            runtime::ReviaSessionTestAccess::Quiet(session);
        }
    } quiet{session};
    std::string id, error;
    Check(session.ProposePresentationChange(id, error), error);
    Check(session.ReviewPresentationChange(true, error), error);
    Check(session.ReviewPresentationChange(false, error), error);
    const auto snapshot = session.DevelopmentStudio();
    Check(snapshot && snapshot->accepted && snapshot->validation && !snapshot->candidate.origin.fixture &&
              !snapshot->candidate.origin.stamp.taskId.empty() && !snapshot->candidate.origin.stamp.attemptId.empty(),
        "Actual Runtime proposal/validation/model review did not establish exact provenance and acceptance.");
    const auto& candidate = snapshot->candidate;
    const auto artifactRoot = session.Paths().Resolve("RuntimeData/Improvement/ClosedPresentation");
    Check(audit::ContentDigest(Read(artifactRoot / id / "validated-discovery.json")) == snapshot->validation->registryDigest,
        "Runtime registry receipt does not match actual host CTest discovery bytes.");
    const auto projected = runtime::ReviaSessionTestAccess::Proposals(session);
    Check(projected.size() == 1 && projected.front().id == "closed-" + id && projected.front().status == ProposalStatus::Verified &&
              projected.front().verificationSummary.find(snapshot->validation->digest) != std::string::npos,
        "Runtime closed evidence was not projected once into the existing ProposalStore.");
    Check(Read(controlled / SelfDevelopment::TargetPath()) == SelfDevelopment::Baseline(), "Production three-stage review changed source.");
    Write(evidence / "production-proposal.json", ToJson(projected.front()).dump(2));
    Write(evidence / "production-review.json", Read(artifactRoot / id / "model-review.json"));
    std::cout << "PASS actual Revia model-origin proposal, native registry and exact companion review project one existing proposal\n";

    LocalLifecycle disposable(session, source, controlled, evidence / "disposable-lifecycle", candidate.origin, true);
    disposable.ctest = ctest;
    DevelopmentCandidate selected;
    Check(disposable.owner->Propose(candidate.change, candidate.origin, selected, error), error);
    Check(disposable.owner->Validate(selected.id, {}, error) && disposable.owner->Review(selected.id, {}, error), error);
    Check(disposable.owner->Integrate(selected.id, error) && disposable.owner->Package(selected.id, error) &&
              disposable.owner->Activate(selected.id, error),
        error);
    const auto privateSentinel = evidence / "private-state.txt";
    Write(privateSentinel, "later private state retained");
    Check(disposable.owner->Recover(selected.id, error), error);
    Check(Read(controlled / SelfDevelopment::TargetPath()) == SelfDevelopment::Baseline() &&
              Read(privateSentinel) == "later private state retained" && disposable.owner->Snapshot(selected.id)->recovered,
        "Measured controlled consumer recovery failed to preserve private state and known-good source.");
    std::cout << "PASS actual disposable native release and deliberately injected consumer-fault recovery\n";

    if (integrateReal)
    {
        Check(Read(source / SelfDevelopment::TargetPath()) == SelfDevelopment::Baseline(),
            "Real helper baseline drifted; integration refused.");
        LocalLifecycle real(session, source, source, evidence / "real-component", candidate.origin, false);
        DevelopmentCandidate accepted;
        Check(real.owner->Propose(candidate.change, candidate.origin, accepted, error), error);
        Check(real.owner->Validate(accepted.id, {}, error) && real.owner->Review(accepted.id, {}, error) &&
                  real.owner->Integrate(accepted.id, error),
            error);
        Check(Read(source / SelfDevelopment::TargetPath()) == accepted.content, "Exact accepted real helper integration did not occur.");
        Write(evidence / "real-integration.json",
            json{{"id", accepted.id}, {"candidate", accepted.candidateDigest}, {"path", SelfDevelopment::TargetPath()},
                {"configuredProvider", candidate.origin.providerIdentity}, {"fixture", false}, {"liveDeployment", false}, {"commit", false}}
                .dump(2));
        std::cout << "PASS exact genuine-model presentation change integrated; executable deployment/commit remains unavailable\n";
    }
    Write(evidence / "result.json",
        json{{"exitCode", 0}, {"model", model}, {"port", port}, {"genuineModelOrigin", true}, {"productionIntegrationAuthority", false},
            {"disposableRecoveryInjection", true}, {"realIntegrated", integrateReal}}
            .dump(2));
}
}

int main(int argc, char** argv)
{
    try
    {
        TestControlledSessionAndTaskRegistration();
        TestProductionMissingScopeRefusesBeforeModel();
        if (argc == 1)
            return 0;
        Check(argc == 8 || argc == 9,
            "Usage: --live source-root fresh-evidence-root configured-model port ctest-exe --controlled-owner-scope [--integrate-real]");
        Check(std::string(argv[1]) == "--live" && std::string(argv[7]) == "--controlled-owner-scope",
            "Explicit trusted launcher scope is required; ordinary model/UI text is not maintenance authorization.");
        const bool real = argc == 9 && std::string(argv[8]) == "--integrate-real";
        Check(argc != 9 || real, "Unknown live stage option.");
        RunLive(actions::Utf8ToPath(argv[2]), actions::Utf8ToPath(argv[3]), argv[4], std::stoi(argv[5]),
            actions::Utf8ToPath(argv[6]).wstring(), real);
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
