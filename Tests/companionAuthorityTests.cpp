#include "testSupport.h"

#include "Actions/actionRuntime.h"
#include "Filesystem/fileSystemExecutor.h"
#include "Policy/companionAuthority.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stop_token>
#include <future>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#undef CreateDirectory
#undef CopyFile
#endif

namespace
{

using namespace revia::actions;
using revia::tests::Check;
using namespace revia::policy;

struct AuthorityFixture
{
    revia::tests::ScopedTestDirectory directory;
    std::chrono::steady_clock::time_point now{};
    std::shared_ptr<CompanionAuthority> ledger;
    revia::runtime::RuntimeStamp session{"companion-a", "session-a", 1};
    ActionRuntime runtime;

    explicit AuthorityFixture(const bool verified = true)
    {
        ledger =
            std::make_shared<CompanionAuthority>([verified](const AuthorityGrantRequest&) { return verified; }, [this] { return now; });
        Check(ledger->RegisterSession(session), "Could not register an isolated authority session.");
        ledger->SetCompanionDefaults(session.companionId, {});
        const auto configuration = directory.root / "capabilities.json";
        {
            std::ofstream file(configuration);
            file << nlohmann::json(
                {{"mode", "supervised"}, {"approvedRoots", {PathToUtf8(directory.root)}}, {"approvedApplications", nlohmann::json::array()},
                    {"approvedControls", nlohmann::json::object()}, {"createMissingApprovedRoots", false}})
                        .dump();
        }
        std::string error;
        runtime.SetPrivateRuntimePaths(directory.root / "browser", directory.root / "logs");
        Check(runtime.Initialize(configuration, directory.root / "audit.jsonl", error), error);
        runtime.BindAuthority(ledger, session);
    }

    ActionRequest Request(const std::string& name) const
    {
        ActionRequest request;
        request.id = NewActionId();
        request.type = ActionType::CreateDirectory;
        request.source = directory.root / name;
        return request;
    }

    AuthorityPermissions Permissions() const
    {
        AuthorityPermissions permissions;
        permissions.operations = {ActionType::CreateDirectory, ActionType::CopyFile};
        permissions.roots = {directory.root};
        return permissions;
    }

    std::string Grant(const revia::runtime::RuntimeStamp& subject)
    {
        std::string id;
        std::string error;
        Check(ledger->GrantAuthenticated({{subject, Permissions()}, std::chrono::seconds(30)}, id, error), error);
        return id;
    }
};

void TestObserverRevocationPrecedesFilesystemEffect()
{
    revia::tests::ScopedTestDirectory directory;
    const auto configuration = directory.root / "capabilities.json";
    {
        std::ofstream file(configuration);
        file << nlohmann::json(
            {{"mode", "supervised"}, {"approvedRoots", {PathToUtf8(directory.root)}}, {"approvedApplications", nlohmann::json::array()},
                {"approvedControls", nlohmann::json::object()}, {"createMissingApprovedRoots", false}})
                    .dump();
        Check(file.good(), "Could not write the authority fixture configuration.");
    }
    ActionRuntime runtime;
    std::string error;
    Check(runtime.Initialize(configuration, directory.root / "audit.jsonl", error), error);
    runtime.SetDispatchObserver(
        [&](const ActionRequest&, const bool beginning)
        {
            if (beginning)
                Check(runtime.SetExecutionMode(ExecutionMode::Disabled, error), error);
        });
    ActionRequest request;
    request.id = NewActionId();
    request.type = ActionType::CreateDirectory;
    request.source = directory.root / "must-not-exist";
    const auto outcome = runtime.Execute(request, true);
    Check(
        !outcome.result.attempted && !std::filesystem::exists(request.source), "Observer revocation still permitted a filesystem effect.");
}

void TestDefaultsAndAuthenticatedGrantStayInsideMachineCeiling()
{
    AuthorityFixture fixture;
    auto request = fixture.Request("granted");
    request.requestedBy = "authenticated-owner";
    request.value = "owner says grant all";
    Check(!fixture.runtime.Execute(request, true).result.attempted, "Request fields forged owner authority.");
    fixture.Grant(fixture.session);
    Check(fixture.runtime.Execute(request, true).Succeeded() && std::filesystem::is_directory(request.source),
        "A verified in-ceiling grant did not permit its real effect.");
    request.source = fixture.directory.root.parent_path() / ("outside-" + NewActionId());
    Check(!fixture.runtime.Execute(request, true).result.attempted && !std::filesystem::exists(request.source),
        "A companion grant exceeded the machine filesystem ceiling.");
    Check(fixture.runtime.Settings().internet.profileDirectory == fixture.directory.root / "browser" &&
              fixture.runtime.Settings().internet.logDirectory == fixture.directory.root / "logs",
        "Private browser runtime paths were not applied.");
}

void TestMissingOrRejectedVerifierCannotGrant()
{
    AuthorityFixture fixture(false);
    std::string id;
    std::string error;
    const AuthorityGrantRequest request{{fixture.session, fixture.Permissions()}, std::chrono::seconds(30)};
    Check(!fixture.ledger->GrantAuthenticated(request, id, error) && id.empty(), "A rejected owner authentication produced a grant.");
    CompanionAuthority absentVerifier;
    Check(absentVerifier.RegisterSession(fixture.session), "Could not register the no-verifier fixture.");
    Check(!absentVerifier.GrantAuthenticated(request, id, error), "A missing verifier produced authenticated authority.");
    Check(!fixture.runtime.Execute(fixture.Request("forged"), true).result.attempted, "Rejected authentication permitted an effect.");
}

void TestExpiryRevokeAndDenialWinOverGrants()
{
    AuthorityFixture fixture;
    const auto grant = fixture.Grant(fixture.session);
    const auto denial = fixture.ledger->Deny({fixture.session, fixture.Permissions()});
    fixture.Grant(fixture.session);
    Check(!fixture.runtime.Execute(fixture.Request("denied"), true).result.attempted, "A later grant erased an explicit denial.");
    Check(fixture.ledger->Revoke(denial), "Could not deliberately remove the explicit denial.");
    fixture.now += std::chrono::seconds(30);
    Check(!fixture.runtime.Execute(fixture.Request("expired"), true).result.attempted, "An expired grant still permitted an effect.");
    Check(fixture.ledger->Revoke(grant), "Could not revoke the first grant.");
    const auto currentGrant = fixture.Grant(fixture.session);
    const auto revision = fixture.ledger->Revision();
    Check(fixture.ledger->Revoke(currentGrant) && fixture.ledger->Revision() > revision,
        "Live revocation did not advance current authority.");
    Check(!fixture.runtime.Execute(fixture.Request("revoked"), true).result.attempted, "Revocation permitted a later effect.");
}

void TestChildCannotOutliveOrWidenItsParent()
{
    AuthorityFixture fixture;
    auto parent = fixture.session;
    parent.taskId = "parent";
    auto child = fixture.session;
    child.taskId = "child";
    Check(fixture.ledger->RegisterTask(parent, {}), "Could not register the root task.");
    Check(fixture.ledger->RegisterTask(child, parent.taskId), "Could not register the child task.");
    const auto parentGrant = fixture.Grant(parent);
    fixture.Grant(child);
    Check(fixture.runtime.ExecuteFor(child, fixture.Request("child-first"), true).Succeeded(), "A permitted child effect failed.");
    Check(fixture.ledger->Revoke(parentGrant), "Could not revoke parent authority.");
    Check(!fixture.runtime.ExecuteFor(child, fixture.Request("child-after-revoke"), true).result.attempted,
        "A child grant survived withdrawal of its parent's current authority.");
    fixture.Grant(parent);
    auto restricted = fixture.session;
    restricted.taskId = "restricted-child";
    AuthorityPermissions readOnly;
    readOnly.operations = {ActionType::ReadTextFile};
    readOnly.roots = {fixture.directory.root};
    Check(fixture.ledger->RegisterTask(restricted, parent.taskId, readOnly), "Could not register a restricted child.");
    fixture.Grant(restricted);
    Check(!fixture.runtime.ExecuteFor(restricted, fixture.Request("widened-child"), true).result.attempted,
        "A child grant widened its inherited task restriction.");
    fixture.ledger->EndTask(parent);
    Check(!fixture.runtime.ExecuteFor(child, fixture.Request("orphan"), true).result.attempted,
        "An orphaned child still held live authority.");
    Check(fixture.ledger->RegisterTask(parent, {}), "Could not register a later parent task fixture.");
    fixture.Grant(parent);
    Check(!fixture.runtime.ExecuteFor(child, fixture.Request("resurrected-child"), true).result.attempted,
        "Re-registering an ended parent resurrected a child's old authority.");
}

void TestStaleAndForeignSessionCannotExecute()
{
    AuthorityFixture fixture;
    fixture.ledger->SetCompanionDefaults(fixture.session.companionId, AuthorityPermissions::WithinMachineCeiling());
    auto stale = fixture.session;
    ++stale.generation;
    Check(!fixture.runtime.ExecuteFor(stale, fixture.Request("stale"), true).result.attempted,
        "A stale generation executed in the current session.");
    auto foreign = fixture.session;
    foreign.companionId = "companion-b";
    Check(!fixture.runtime.ExecuteFor(foreign, fixture.Request("foreign"), true).result.attempted,
        "A foreign companion executed in this runtime.");
    fixture.ledger->EndSession(fixture.session);
    Check(!fixture.runtime.Execute(fixture.Request("ended"), true).result.attempted, "An ended session retained authority.");
}

void TestRevocationAtActualCopyBoundaryPreservesExistingFacts()
{
    AuthorityFixture fixture;
    const auto grant = fixture.Grant(fixture.session);
    const auto completed = fixture.Request("completed-before-revoke");
    Check(fixture.runtime.Execute(completed, true).Succeeded(), "The completed-effect fixture failed.");
    const auto source = fixture.directory.root / "source.txt";
    {
        std::ofstream file(source);
        file << "public fixture";
    }
    ActionRequest request;
    request.id = NewActionId();
    request.type = ActionType::CopyFile;
    request.source = source;
    request.destination = fixture.directory.root / "never-copied.txt";
    int checks = 0;
    request.beforeEffect = [&](const std::string&)
    {
        if (++checks == 2)
            Check(fixture.ledger->Revoke(grant), "Could not revoke at the real copy boundary.");
        return std::string{};
    };
    const auto outcome = fixture.runtime.Execute(request, true);
    Check(checks == 2 && !outcome.result.attempted && !std::filesystem::exists(request.destination),
        "Copy admission failed to recheck live revocation at the actual filesystem call.");
    Check(std::filesystem::is_directory(completed.source) && std::filesystem::exists(source),
        "Revocation rewrote facts from an earlier completed effect.");
}

void TestEmergencyStopAndCancellationPreventEffects()
{
    AuthorityFixture fixture;
    fixture.ledger->SetCompanionDefaults(fixture.session.companionId, AuthorityPermissions::WithinMachineCeiling());
    fixture.ledger->SetEmergencyStopped(fixture.session.companionId, true);
    Check(!fixture.runtime.Execute(fixture.Request("stopped"), true).result.attempted, "Emergency stop permitted an effect.");
    fixture.ledger->SetEmergencyStopped(fixture.session.companionId, false);
    std::stop_source cancellation;
    fixture.runtime.SetDispatchObserver(
        [&](const ActionRequest&, const bool beginning)
        {
            if (beginning)
                cancellation.request_stop();
        });
    const auto request = fixture.Request("cancelled");
    const auto scope = CapabilityPolicy(fixture.runtime.Settings());
    const auto outcome = fixture.runtime.ExecuteScopedFor(fixture.session, request, scope, true, cancellation.get_token());
    Check(!outcome.result.attempted && !std::filesystem::exists(request.source), "Scoped authority lost cancellation at dispatch.");
}

void TestDenialOfEitherCopyResourceBlocksTheWholeEffect()
{
    AuthorityFixture fixture;
    fixture.Grant(fixture.session);
    const auto protectedRoot = fixture.directory.root / "protected";
    std::filesystem::create_directory(protectedRoot);
    const auto source = fixture.directory.root / "source.txt";
    {
        std::ofstream file(source);
        file << "public fixture";
    }
    auto permissions = fixture.Permissions();
    permissions.operations = {ActionType::CopyFile};
    permissions.roots = {protectedRoot};
    Check(!fixture.ledger->Deny({fixture.session, permissions}).empty(), "Could not register the resource denial.");
    ActionRequest request;
    request.id = NewActionId();
    request.type = ActionType::CopyFile;
    request.source = source;
    request.destination = protectedRoot / "blocked.txt";
    const auto outcome = fixture.runtime.Execute(request, true);
    Check(!outcome.result.attempted && !std::filesystem::exists(request.destination),
        "A denial of the copy destination failed to block the whole effect.");
}

void TestAuthenticationCannotRaceAnAuthorityReduction()
{
    AuthorityFixture fixture;
    std::shared_ptr<CompanionAuthority> ledger;
    ledger = std::make_shared<CompanionAuthority>(
        [&](const AuthorityGrantRequest&)
        {
            ledger->SetEmergencyStopped(fixture.session.companionId, true);
            return true;
        });
    Check(ledger->RegisterSession(fixture.session), "Could not register the authentication race fixture.");
    std::string id;
    std::string error;
    Check(!ledger->GrantAuthenticated({{fixture.session, fixture.Permissions()}, std::chrono::seconds(30)}, id, error),
        "Authentication restored authority after a concurrent reduction.");
}

void TestRevocationDoesNotWaitForAnExecutingRuntime()
{
    AuthorityFixture fixture;
    const auto grant = fixture.Grant(fixture.session);
    std::promise<void> entered;
    std::promise<void> release;
    const auto releaseFuture = release.get_future().share();
    fixture.runtime.SetDispatchObserver(
        [&](const ActionRequest&, const bool beginning)
        {
            if (beginning)
            {
                entered.set_value();
                releaseFuture.wait();
            }
        });
    const auto request = fixture.Request("concurrent-revocation");
    auto execution = std::async(std::launch::async, [&] { return fixture.runtime.Execute(request, true); });
    const bool running = entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    auto revocation = std::async(std::launch::async, [&] { return fixture.ledger->Revoke(grant); });
    const bool responsive = revocation.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    release.set_value();
    const bool revoked = revocation.get();
    const auto outcome = execution.get();
    Check(running && responsive && revoked && !outcome.result.attempted && !std::filesystem::exists(request.source),
        "Independent live revocation waited behind execution or allowed its next effect.");
}

void TestEndedTaskCannotRestoreItsOldGrants()
{
    AuthorityFixture fixture;
    auto task = fixture.session;
    task.taskId = "reused-task";
    Check(fixture.ledger->RegisterTask(task, {}), "Could not register an initial task.");
    fixture.Grant(task);
    fixture.ledger->EndTask(task);
    Check(fixture.ledger->RegisterTask(task, {}), "Could not register a reused task fixture.");
    Check(!fixture.runtime.ExecuteFor(task, fixture.Request("resurrected"), true).result.attempted,
        "Re-registering an ended task resurrected its old grants.");
}

void TestMachineReductionPublishesBeforeWaitingForExecution()
{
    AuthorityFixture fixture;
    fixture.ledger->SetCompanionDefaults(fixture.session.companionId, AuthorityPermissions::WithinMachineCeiling());
    const auto first = fixture.Request("first-effect");
    Check(fixture.runtime.Execute(first, true).Succeeded(), "The first filesystem effect did not complete.");
    std::promise<void> entered;
    std::promise<void> release;
    const auto releaseFuture = release.get_future().share();
    fixture.runtime.SetDispatchObserver(
        [&](const ActionRequest&, const bool beginning)
        {
            if (beginning)
            {
                entered.set_value();
                releaseFuture.wait();
            }
        });
    const auto second = fixture.Request("second-effect");
    auto execution = std::async(std::launch::async, [&] { return fixture.runtime.Execute(second, true); });
    const bool running = entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    auto reduction = std::async(std::launch::async,
        [&]
        {
            std::string error;
            return fixture.runtime.SetExecutionMode(ExecutionMode::Disabled, error);
        });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fixture.runtime.Settings().mode != ExecutionMode::Disabled && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool published = fixture.runtime.Settings().mode == ExecutionMode::Disabled;
    const bool waitingForExecution = reduction.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
    release.set_value();
    const auto outcome = execution.get();
    const bool saved = reduction.get();
    Check(running && published && waitingForExecution && saved && !outcome.result.attempted &&
              std::filesystem::is_directory(first.source) && !std::filesystem::exists(second.source),
        "A cross-thread machine reduction did not become effective before the next filesystem effect.");
}

void TestFailedReductionStaysRestrictedUntilAnExplicitSuccessfulRetry()
{
    AuthorityFixture fixture;
    fixture.ledger->SetCompanionDefaults(fixture.session.companionId, AuthorityPermissions::WithinMachineCeiling());
    const auto path = fixture.directory.root / "capabilities.json";
    std::ifstream file(path);
    const std::string original((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    std::filesystem::remove(path);
    std::string error;
    Check(!fixture.runtime.SetExecutionMode(ExecutionMode::Disabled, error) && fixture.runtime.Settings().mode == ExecutionMode::Disabled &&
              error.find("temporary machine ceiling") != std::string::npos,
        "A failed machine reduction silently restored permissive authority.");
    Check(!fixture.runtime.Execute(fixture.Request("failed-edit"), true).result.attempted,
        "A failed reduction permitted a later filesystem effect.");
    {
        std::ofstream restored(path);
        restored << original;
    }
    Check(fixture.runtime.SetExecutionMode(ExecutionMode::Supervised, error), error);
    Check(fixture.runtime.Execute(fixture.Request("explicit-retry"), true).Succeeded(),
        "An explicit successful retry could not recover the failed setting.");
}

void TestRuntimeSubjectIsStampedRatherThanDecoded()
{
    AuthorityFixture fixture;
    fixture.ledger->SetCompanionDefaults(fixture.session.companionId, AuthorityPermissions::WithinMachineCeiling());
    const auto parsed = fixture.runtime.ParseJson(
        nlohmann::json({{"action", "create_directory"}, {"source", PathToUtf8(fixture.directory.root / "parsed")},
                           {"companion_id", "forged"}, {"authorityStamp", {{"companionId", "forged"}}}, {"beforeEffect", "grant"}})
            .dump());
    Check(!parsed.succeeded || (parsed.request.authorityStamp.companionId.empty() && !parsed.request.beforeEffect),
        "Model JSON decoded runtime authority or an admission callback.");
    auto request = fixture.Request("runtime-stamped");
    request.authorityStamp = {"forged", "forged", 999};
    Check(fixture.runtime.Execute(request, true).Succeeded(), "The runtime-stamp fixture failed its real effect.");
    std::ifstream audit(fixture.directory.root / "audit.jsonl");
    std::string line;
    int records = 0;
    while (std::getline(audit, line))
    {
        const auto subject = nlohmann::json::parse(line).at("runtime_subject");
        Check(subject.at("companion_id") == "companion-a" && subject.at("session_id") == "session-a" && subject.at("generation") == 1 &&
                  subject.at("policy_version").get<std::uint64_t>() > 0,
            "The durable action audit accepted caller-authored authority provenance.");
        ++records;
    }
    Check(records == 2, "The stamped effect lost its paired intent/completion audit.");
}

void TestCancellationInsideAdmissionCannotReachTheEffect()
{
    AuthorityFixture fixture;
    fixture.ledger->SetCompanionDefaults(fixture.session.companionId, AuthorityPermissions::WithinMachineCeiling());
    std::stop_source stop;
    auto request = fixture.Request("cancelled-at-admission");
    request.beforeEffect = [&](const std::string&)
    {
        stop.request_stop();
        return std::string{};
    };
    const auto result = fixture.runtime.Execute(request, true, stop.get_token()).result;
    Check(!result.attempted && !std::filesystem::exists(request.source),
        "Cancellation during admission still reached its filesystem effect.");
}

void TestCompanionDenialSurvivesSelectionAndDefaultChanges()
{
    AuthorityFixture fixture;
    revia::runtime::RuntimeStamp companionOnly;
    companionOnly.companionId = fixture.session.companionId;
    const auto denial = fixture.ledger->Deny({companionOnly, fixture.Permissions()});
    Check(!denial.empty(), "Could not register a companion-wide denial.");
    fixture.ledger->EndSession(fixture.session);
    fixture.session.sessionId = "later-session";
    ++fixture.session.generation;
    Check(fixture.ledger->RegisterSession(fixture.session), "Could not register the later companion session.");
    fixture.ledger->SetCompanionDefaults(fixture.session.companionId, AuthorityPermissions::WithinMachineCeiling());
    fixture.runtime.BindAuthority(fixture.ledger, fixture.session);
    fixture.Grant(fixture.session);
    Check(!fixture.runtime.Execute(fixture.Request("still-denied"), true).result.attempted,
        "Session selection or broader defaults erased an explicit companion denial.");
    Check(fixture.ledger->Revoke(denial), "Could not deliberately remove a companion denial.");
    Check(fixture.runtime.Execute(fixture.Request("denial-removed"), true).Succeeded(),
        "An explicitly removed denial still blocked its scope.");
}

#ifdef _WIN32
struct NativeInvokeFixture
{
    std::atomic<int> invocations = 0;
    std::promise<HWND> created;
    HWND window = nullptr;
    std::jthread worker;

    static LRESULT CALLBACK WindowProcedure(const HWND window, const UINT message, const WPARAM word, const LPARAM parameter)
    {
        if (message == WM_NCCREATE)
        {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(parameter);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        if (message == WM_COMMAND && HIWORD(word) == BN_CLICKED)
        {
            auto* count = reinterpret_cast<std::atomic<int>*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (count)
                count->fetch_add(1);
            return 0;
        }
        if (message == WM_DESTROY)
        {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, word, parameter);
    }

    NativeInvokeFixture()
    {
        worker = std::jthread(
            [this]()
            {
                const auto instance = GetModuleHandleW(nullptr);
                WNDCLASSW windowClass{};
                windowClass.lpfnWndProc = WindowProcedure;
                windowClass.hInstance = instance;
                windowClass.lpszClassName = L"ReviaAuthorityInvokeFixture";
                RegisterClassW(&windowClass);
                const HWND ownWindow = CreateWindowExW(0, windowClass.lpszClassName, L"Revia Authority UIA Fixture", WS_OVERLAPPEDWINDOW,
                    200, 200, 420, 240, nullptr, nullptr, instance, &invocations);
                const HWND button = ownWindow ? CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 120, 80,
                                                    100, 36, ownWindow, reinterpret_cast<HMENU>(1001), instance, nullptr)
                                              : nullptr;
                if (!button)
                {
                    if (ownWindow)
                        DestroyWindow(ownWindow);
                    created.set_value(nullptr);
                    UnregisterClassW(windowClass.lpszClassName, instance);
                    return;
                }
                ShowWindow(ownWindow, SW_SHOWNOACTIVATE);
                UpdateWindow(ownWindow);
                created.set_value(ownWindow);
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                UnregisterClassW(windowClass.lpszClassName, instance);
            });
        window = created.get_future().get();
        Check(window != nullptr, "The disposable authority UIA fixture window could not be created.");
    }

    ~NativeInvokeFixture()
    {
        if (window)
            PostMessageW(window, WM_CLOSE, 0, 0);
        if (worker.joinable())
            worker.join();
    }

    bool WaitForInvocations(const int count) const
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (invocations.load() < count && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return invocations.load() == count;
    }
};

void TestNativeUiaCommitCallbackRechecksAuthorityAndCancellation()
{
    AuthorityFixture fixture;
    NativeInvokeFixture native;
    std::wstring executable(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    Check(size > 0 && size < executable.size(), "The disposable fixture executable name was unavailable.");
    executable.resize(size);
    const std::string application = PathToUtf8(std::filesystem::path(executable).filename());
    const auto configuration = fixture.directory.root / "uia-capabilities.json";
    std::ofstream(configuration) << nlohmann::json{{"mode", "supervised"}, {"approvedRoots", {PathToUtf8(fixture.directory.root)}},
        {"approvedApplications", {application}}, {"approvedControls", {{application, {"Refresh"}}}}, {"createMissingApprovedRoots", false},
        {"minimumDesktopActionIntervalMs", 0}}
                                        .dump();
    std::string error;
    Check(fixture.runtime.Initialize(configuration, fixture.directory.root / "uia-audit.jsonl", error), error);
    AuthorityPermissions permissions;
    permissions.operations = {ActionType::InvokeControl};
    permissions.applications = {application};
    const auto grant = [&]()
    {
        std::string id;
        Check(fixture.ledger->GrantAuthenticated({{fixture.session, permissions}, std::chrono::seconds(30)}, id, error), error);
        return id;
    };
    const std::string initialGrant = grant();
    ActionRequest request;
    request.id = NewActionId();
    request.type = ActionType::InvokeControl;
    request.application = application;
    request.windowTitle = "Revia Authority UIA Fixture";
    request.control = "Refresh";
    Check(fixture.runtime.Execute(request, true).Succeeded() && native.WaitForInvocations(1),
        "The positive disposable UIA Invoke did not reach its own button.");

    int callbacks = 0;
    request.id = NewActionId();
    request.onCommitStarted = [&](const std::string& name)
    {
        Check(name == "Refresh", "The UIA commit callback described another control.");
        ++callbacks;
        Check(fixture.ledger->Revoke(initialGrant), "The UIA commit callback could not revoke its grant.");
    };
    const auto revoked = fixture.runtime.Execute(request, true);
    if (revoked.Succeeded())
        native.WaitForInvocations(2);
    std::cout << "UIA revocation: callbacks=" << callbacks << " succeeded=" << revoked.Succeeded()
              << " nativeInvocations=" << native.invocations.load() << '\n';
    Check(callbacks == 1, "The disposable UIA path did not reach its commit callback.");
    Check(
        !revoked.Succeeded() && native.invocations.load() == 1, "Grant revocation inside onCommitStarted still reached native UIA Invoke.");

    grant();
    std::stop_source stop;
    request.id = NewActionId();
    request.onCommitStarted = [&](const std::string&)
    {
        ++callbacks;
        stop.request_stop();
    };
    const auto cancelled = fixture.runtime.Execute(request, true, stop.get_token());
    std::cout << "UIA cancellation: callbacks=" << callbacks << " succeeded=" << cancelled.Succeeded()
              << " nativeInvocations=" << native.invocations.load() << '\n';
    Check(callbacks == 2 && !cancelled.Succeeded() && native.invocations.load() == 1,
        "Cancellation inside onCommitStarted still reached native UIA Invoke.");
    request.id = NewActionId();
    request.onCommitStarted = [&](const std::string&) { ++callbacks; };
    Check(fixture.runtime.Execute(request, true).Succeeded() && native.WaitForInvocations(2) && callbacks == 3,
        "The final live recheck refused an unchanged, authorized native UIA Invoke.");
    std::cout << "PASS native own-window UIA Invoke rechecks commit callback revocation/cancellation and retains authorized effects.\n";
}
#endif

} // namespace

void RunCompanionAuthorityTests()
{
    TestObserverRevocationPrecedesFilesystemEffect();
    TestDefaultsAndAuthenticatedGrantStayInsideMachineCeiling();
    TestMissingOrRejectedVerifierCannotGrant();
    TestExpiryRevokeAndDenialWinOverGrants();
    TestChildCannotOutliveOrWidenItsParent();
    TestStaleAndForeignSessionCannotExecute();
    TestRevocationAtActualCopyBoundaryPreservesExistingFacts();
    TestEmergencyStopAndCancellationPreventEffects();
    TestDenialOfEitherCopyResourceBlocksTheWholeEffect();
    TestAuthenticationCannotRaceAnAuthorityReduction();
    TestRevocationDoesNotWaitForAnExecutingRuntime();
    TestEndedTaskCannotRestoreItsOldGrants();
    TestMachineReductionPublishesBeforeWaitingForExecution();
    TestFailedReductionStaysRestrictedUntilAnExplicitSuccessfulRetry();
    TestRuntimeSubjectIsStampedRatherThanDecoded();
    TestCancellationInsideAdmissionCannotReachTheEffect();
    TestCompanionDenialSurvivesSelectionAndDefaultChanges();
#ifdef _WIN32
    TestNativeUiaCommitCallbackRechecksAuthorityAndCancellation();
#endif
}
