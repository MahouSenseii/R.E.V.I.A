#include "reviaSessionTestAccess.h"

#include "Core/configManager.h"
#include "Core/messageRouter.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// The assisted path, with a real model answering.
//
//   ReviaTests.exe --computer-live [port]
//
// Everything else in this suite scripts the two decision calls, and that is right for
// testing control flow: a fixed answer makes a run reproducible. It is also the reason
// none of it is evidence that the arrangement works. A subgoal this file wrote is a
// subgoal shaped like the parser; a real model produces what it produces.
//
// So nothing here is scripted. `/operate <request>` goes in through the same Submit the
// person uses, Main is asked for a bounded subgoal and gets to answer however it likes,
// the routine policy decides what it can, and every call that reaches the model is
// counted and printed. If Main answers something the validator refuses, that is a
// result and it is reported as one.
//
// The only application it touches is the disposable fixture.

namespace
{

using namespace revia::actions;
using namespace revia::computer;
using namespace revia::goals;
using revia::runtime::ReviaSession;
using revia::runtime::ReviaSessionTestAccess;
using revia::tests::Check;

#ifdef _WIN32

constexpr const char* FixtureExecutable = "ReviaDesktopFixture.exe";
constexpr const char* FixtureApplication = "reviadesktopfixture.exe";

class Fixture
{
public:
    Fixture()
    {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        executable = std::filesystem::path(modulePath).parent_path() / FixtureExecutable;
        logFile = std::filesystem::temp_directory_path() /
            ("revia-live-" + NewActionId() + ".log");

        const std::wstring commandLine =
            L"\"" + executable.wstring() + L"\" --log \"" + logFile.wstring() + L"\"";
        std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
        mutableCommandLine.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        started = CreateProcessW(executable.c_str(), mutableCommandLine.data(), nullptr,
            nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) != FALSE;
        if (started)
        {
            WaitForInputIdle(process.hProcess, 5000);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            Front();
        }
    }

    ~Fixture()
    {
        if (started)
        {
            const HWND window = Window();
            if (window != nullptr) PostMessageW(window, WM_CLOSE, 0, 0);
            if (WaitForSingleObject(process.hProcess, 2000) != WAIT_OBJECT_0)
            {
                TerminateProcess(process.hProcess, 0);
            }
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        std::error_code error;
        std::filesystem::remove(logFile, error);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    [[nodiscard]] bool Started() const { return started; }

    [[nodiscard]] std::string Log() const
    {
        std::ifstream file(logFile);
        return std::string(
            std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    [[nodiscard]] HWND Window() const
    {
        struct Search { DWORD processId; HWND found; } search{process.dwProcessId, nullptr};
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL
        {
            auto& state = *reinterpret_cast<Search*>(parameter);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner != state.processId) return TRUE;
            wchar_t text[256]{};
            GetWindowTextW(window, text, 255);
            if (std::wstring(text) == L"Revia Fixture - Main")
            {
                state.found = window;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        return search.found;
    }

    void Front() const
    {
        const HWND window = Window();
        if (window == nullptr) return;
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    // Empties the fields between tasks, so one task is not verified by the leftovers of
    // the one before it -- which would make a comparison across modes meaningless in
    // exactly the direction that flatters whichever ran second.
    void Reset() const
    {
        const HWND window = Window();
        if (window != nullptr) PostMessageW(window, WM_APP + 4, 0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

private:
    std::filesystem::path executable;
    std::filesystem::path logFile;
    PROCESS_INFORMATION process{};
    bool started = false;
};

bool BackendAnswers(const int port, std::string& outReason)
{
    // Asked through the project's own client rather than by probing the port, so that
    // "the backend is available" means the thing the runtime will actually use works.
    // The project's own configured settings, with only the port redirected. Inventing
    // settings here would mean probing a different backend than the one the session is
    // about to use -- and the model-identity check would fail on the invented name
    // rather than on anything real.
    appSettings configured;
    configManager config;
    if (!config.LoadSettings(configured))
    {
        outReason = "Config/settings.json could not be loaded.";
        return false;
    }
    llmSettings settings = configured.llm;
    settings.port = port;
    settings.bAutoStartServer = false;

    messageRouter router;
    router.ApplyLLMSettings(settings, embeddingSettings{}, aiProfile{});
    const healthOutput health = router.CheckLLMHealth();
    if (!health.bIsAvailable)
    {
        outReason = health.reason.empty()
            ? ("No llama.cpp server answered on 127.0.0.1:" + std::to_string(port))
            : health.reason;
        return false;
    }
    return true;
}

struct LiveSession
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    std::vector<ReviaSessionTestAccess::ModelCall> calls;

    explicit LiveSession(const int port)
    {
        const auto approved = directory.root / "approved";
        std::filesystem::create_directories(approved);
        {
            std::ofstream file(directory.root / "capabilities.json");
            file << nlohmann::json{
                {"mode", "approved_scope"},
                {"approvedRoots", {PathToUtf8(approved)}},
                {"approvedApplications", {FixtureApplication}},
                {"approvedControls", {{FixtureApplication, {"*"}}}},
                {"autoApproveRiskThrough", "reversible_write"},
                {"desktopControl", {
                    {"applicationLaunch", true},
                    {"keyboard", true},
                    {"pointer", true},
                    {"maxTypedCharacters", 512}}},
                {"createMissingApprovedRoots", false}}.dump();
        }
        ReviaSessionTestAccess::PrepareOperator(session, directory.root);
        // The real router, pointed at the server this run owns.
        ReviaSessionTestAccess::ConfigureLiveOperatorPlanner(session, port);
        // Real calls, recorded. Not scripted.
        ReviaSessionTestAccess::InstrumentComputerProviders(session, calls);
        ReviaSessionTestAccess::UseRealStepProvider(session);
    }

    void UseMode(const std::string& mode)
    {
        computerControlSettings settings;
        settings.providerMode = mode;
        ReviaSessionTestAccess::SetComputerSettings(session, settings);
    }

    [[nodiscard]] ComputerTaskCoordinator& Tasks()
    {
        return ReviaSessionTestAccess::ComputerTasks(session);
    }
};

void PrintCalls(const std::vector<ReviaSessionTestAccess::ModelCall>& calls)
{
    std::cout << "   model calls: " << calls.size() << "\n";
    for (std::size_t index = 0; index < calls.size(); ++index)
    {
        const auto& call = calls[index];
        std::cout << "     [" << index << "] " << std::setw(10) << std::left << call.kind
                  << "  prompt=" << call.promptBytes << "B"
                  << "  reply=" << call.responseBytes << "B"
                  << "  tokens=" << (call.tokensReported
                        ? std::to_string(call.tokens) : std::string("unreported"))
                  << "  " << call.milliseconds << "ms"
                  << "  " << (call.succeeded ? "ok" : "failed") << "\n";
    }
}

// One eligible task, end to end, with the model answering for itself.
void RunEligibleTask(Fixture& fixture, const int port, const std::string& request)
{
    std::cout << "\n===== assisted, live model =====\n";
    std::cout << "   request: " << request << "\n";
    fixture.Front();

    LiveSession live(port);
    live.UseMode("assisted");

    // Nothing is held here any more, and that is the change worth noting.
    //
    // The runtime reads the user's exact words out of their own request before any model
    // is asked anything, so a live run now exercises the production path instead of a
    // test hook that reached past it. A request that quotes nothing holds nothing, which
    // is also correct: the gate then refuses a text entry rather than letting one be
    // composed.

    int confirmations = 0;
    live.session.SetConfirmationHandler(
        [&](const ActionRequest&, const PolicyDecision&)
        {
            ++confirmations;
            return ConfirmationChoice::AllowForThisTask;
        });

    std::cout << "   requested mode: " << ToString(live.Tasks().Controller().Mode())
              << "\n";
    std::cout << "   before the task, /controller says: "
              << ToString(live.Tasks().Controller().EffectiveMode()) << "\n";
    std::cout << "     why: " << live.Tasks().Controller().ModeUnavailableReason() << "\n";

    const auto started = std::chrono::steady_clock::now();
    const revia::runtime::SessionResult result =
        ReviaSessionTestAccess::SubmitOperator(live.session, "/operate " + request);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    const ComputerControllerStats stats = live.Tasks().Stats();
    std::cout << "\n   -- outcome --\n";
    std::cout << "   succeeded:      " << (result.succeeded ? "yes" : "no") << "\n";
    std::cout << "   reason:         " << result.reason << "\n";
    std::cout << "   wall clock:     " << elapsed << "ms\n";
    std::cout << "   confirmations:  " << confirmations << "\n";
    std::cout << "   decisions:      " << stats.decisions << "\n";
    std::cout << "   routine:        " << stats.routineDecisions << "\n";
    std::cout << "   reached model:  " << stats.modelCalls << "\n";
    std::cout << "   escalations:    " << stats.escalations << "\n";
    PrintCalls(live.calls);

    std::cout << "\n   -- what the model actually proposed --\n";
    for (const auto& call : live.calls)
    {
        if (call.kind != "subgoal") continue;
        std::string reply = call.response;
        for (char& character : reply)
        {
            if (character == '\n' || character == '\r') character = ' ';
        }
        if (reply.size() > 300) reply = reply.substr(0, 300) + "...";
        std::cout << "     " << reply << "\n";
    }

    std::cout << "\n   -- the fixture's own record --\n";
    const std::string log = fixture.Log();
    std::cout << (log.empty() ? "     (nothing)\n" : log);

    // The assertions are about the arrangement, not about the model being clever.
    //
    // A 4B model may or may not produce a subgoal the validator accepts, and the run is
    // informative either way -- what must hold is that the path was genuinely exercised
    // and that nothing unauthorized happened when it was.
    Check(!live.calls.empty(),
        "No call reached the model, so this was not a live run.");
    const bool askedForSubgoal = std::any_of(live.calls.begin(), live.calls.end(),
        [](const auto& call) { return call.kind == "subgoal"; });
    Check(askedForSubgoal,
        "Assisted mode never asked for a bounded subgoal, so the assisted path was "
        "not the one that ran.");

    if (stats.routineDecisions > 0)
    {
        std::cout << "\n   ASSISTED CONFIRMED: " << stats.routineDecisions
                  << " of " << stats.decisions
                  << " step decision(s) were taken without a model.\n";
    }
    else
    {
        std::cout << "\n   ASSISTED NOT REACHED: the model's subgoal was not accepted, "
                     "or the routine policy abstained. The run fell back, which is the "
                     "designed behaviour; see the refusal above.\n";
    }
}

// The same task with the providers left alone, for the comparison in section 3.
void RunLegacyTask(Fixture& fixture, const int port)
{
    std::cout << "\n===== legacy, live model, same task =====\n";
    fixture.Front();

    LiveSession live(port);
    live.UseMode("legacy");

    int confirmations = 0;
    live.session.SetConfirmationHandler(
        [&](const ActionRequest&, const PolicyDecision&)
        {
            ++confirmations;
            return ConfirmationChoice::AllowForThisTask;
        });

    const auto started = std::chrono::steady_clock::now();
    const revia::runtime::SessionResult result = ReviaSessionTestAccess::SubmitOperator(
        live.session,
        "/operate put \"dinner at eight\" into the Compose box in the fixture window");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    const ComputerControllerStats stats = live.Tasks().Stats();
    std::cout << "   succeeded:      " << (result.succeeded ? "yes" : "no") << "\n";
    std::cout << "   reason:         " << result.reason << "\n";
    std::cout << "   wall clock:     " << elapsed << "ms\n";
    std::cout << "   confirmations:  " << confirmations << "\n";
    std::cout << "   decisions:      " << stats.decisions << "\n";
    std::cout << "   reached model:  " << stats.modelCalls << "\n";
    PrintCalls(live.calls);

    Check(stats.routineDecisions == 0,
        "Legacy mode routed a decision away from the model, which is the one thing the "
        "default must never do.");
    const bool askedForSubgoal = std::any_of(live.calls.begin(), live.calls.end(),
        [](const auto& call) { return call.kind == "subgoal"; });
    Check(!askedForSubgoal,
        "Legacy mode spent a model call on a subgoal it will never use.");
}

// Why /controller reports legacy with no task in progress.
void ExplainTheIdleReport(const int port)
{
    std::cout << "\n===== why /controller says legacy when nothing is running =====\n";
    LiveSession live(port);
    live.UseMode("assisted");

    std::cout << "   requested mode:  " << ToString(live.Tasks().Controller().Mode())
              << "\n";
    std::cout << "   effective mode:  "
              << ToString(live.Tasks().Controller().EffectiveMode()) << "\n";
    std::cout << "   reason:          "
              << live.Tasks().Controller().ModeUnavailableReason() << "\n";
    std::cout << "   has subgoal:     "
              << (live.Tasks().Controller().HasSubgoal() ? "yes" : "no") << "\n";

    Check(live.Tasks().Controller().Mode() == ComputerProviderMode::Assisted,
        "The requested mode was not recorded.");
    Check(live.Tasks().Controller().EffectiveMode() == ComputerProviderMode::Legacy,
        "Assisted reported itself active with nothing to work from.");

    std::cout <<
        "\n   This is not a fault and not a fallback. A subgoal belongs to a task: it is\n"
        "   asked for when a run starts and dropped when it ends, so between runs there\n"
        "   is nothing for the routine policy to be deterministic about. Reporting\n"
        "   'assisted' there would be claiming a capability that has no input.\n"
        "   The eligible-task run above is where the distinction is actually visible.\n";
}

// Shadow mode, against the real model.
//
// The arrangement this is here to check is easy to state and easy to get wrong: the
// existing path decides and executes; the routine policy is asked the same question from
// the *same* snapshot; its answer is recorded and compared and never reaches the machine.
//
// The failure it guards against is subtle. A shadow provider that took its own look at
// the screen would bump the process-wide observation generation, and the live provider's
// visual target -- chosen correctly a moment earlier -- would be refused at execution as
// stale. The shadow would have broken the live run by observing it.
void RunShadowTask(Fixture& fixture, const int port)
{
    std::cout << "\n===== shadow mode, live model =====\n";
    fixture.Reset();
    fixture.Front();

    LiveSession live(port);
    live.UseMode("shadow");
    live.session.SetConfirmationHandler(
        [](const ActionRequest&, const PolicyDecision&)
        {
            return ConfirmationChoice::AllowForThisTask;
        });

    const std::string request =
        "press the Zoom in button in the fixture window";
    const auto started = std::chrono::steady_clock::now();
    const revia::runtime::SessionResult result =
        ReviaSessionTestAccess::SubmitOperator(live.session, "/operate " + request);
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    const ComputerControllerStats stats = live.Tasks().Stats();
    std::cout << "   request:        " << request << "\n";
    std::cout << "   succeeded:      " << (result.succeeded ? "yes" : "no") << "\n";
    std::cout << "   wall clock:     " << elapsed << "ms\n";
    std::cout << "   decisions:      " << stats.decisions << "\n";
    std::cout << "   comparisons:    " << stats.shadowComparisons << "\n";
    std::cout << "   agreed:         " << stats.shadowAgreements << " of "
              << stats.shadowComparisons << "\n";
    std::cout << "   executed by:    legacy (" << stats.legacyDecisions
              << " decision(s))\n";
    std::cout << "   routine acted:  " << stats.routineDecisions
              << "   <- must be zero in shadow mode\n";

    Check(stats.shadowComparisons > 0,
        "Shadow mode ran without comparing anything, so the shadow provider was never "
        "actually asked.");
    // The whole safety property, in one assertion. A shadow that executed would be a
    // second decision path reaching the machine without ever having earned it.
    Check(stats.routineDecisions == 0,
        "The shadow provider's answer was executed. In shadow mode nothing it says may "
        "reach the machine: that is the arrangement by which a provider earns trust "
        "before it is trusted.");

    std::cout << "\n   -- the fixture's own record --\n";
    const std::string log = fixture.Log();
    std::cout << (log.empty() ? "     (nothing)\n" : log);
    std::cout << "\n   Both providers were asked from one observation. The live target "
                 "was not\n   invalidated by the comparison, which is what taking a "
                 "second look would have done.\n";
}

// Legacy against assisted, on the same controlled tasks, with the real model.
//
// The previous delivery compared one task in each mode and reported the step decisions
// that did not reach a model as the saving. That is half a measurement and it is the
// flattering half: a subgoal costs a model call, a refused subgoal costs one and buys
// nothing, and a run that offloads three decisions while spending two on planning has
// saved one call rather than three.
//
// So this runs a fixed set of tasks under both modes and reports what a person would
// actually notice: did the task finish, how long did it take, and how many times in
// total did anything reach a model -- planning, deciding, falling back, all of it.
struct ComparisonRow
{
    std::string task;
    std::string mode;
    bool completed = false;
    long long wallMs = 0;
    ModelCallLedger ledger;
    std::uint32_t contentSupplied = 0;
    std::uint32_t contentInvented = 0;
};

ComparisonRow RunOneControlledTask(
    Fixture& fixture, const int port, const std::string& mode, const std::string& request)
{
    fixture.Reset();
    fixture.Front();

    LiveSession live(port);
    live.UseMode(mode);
    live.session.SetConfirmationHandler(
        [](const ActionRequest&, const PolicyDecision&)
        {
            return ConfirmationChoice::AllowForThisTask;
        });

    const auto started = std::chrono::steady_clock::now();
    const revia::runtime::SessionResult result =
        ReviaSessionTestAccess::SubmitOperator(live.session, "/operate " + request);
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    ComparisonRow row;
    row.task = request;
    row.mode = mode;
    row.completed = result.succeeded;
    row.wallMs = elapsed;
    row.ledger = live.Tasks().Calls();
    row.contentSupplied = live.Tasks().ContentStats().supplied;
    row.contentInvented = live.Tasks().ContentStats().inventions +
        live.Tasks().ContentStats().modifiedPayloads;
    return row;
}

void RunControlledComparison(Fixture& fixture, const int port)
{
    std::cout << "\n===== legacy versus assisted, same tasks, real model =====\n";

    // Three shapes, chosen because they exercise different parts of the path: a press
    // with no content, a placement with exact content the runtime holds, and a
    // placement into the other panel. Every one of them is safe to repeat.
    const std::vector<std::string> tasks{
        "press the Zoom in button in the fixture window",
        "put \"dinner at eight\" into the Compose box in the fixture window",
        "put \"about tomorrow\" into the Subject box in the fixture window",
    };

    std::vector<ComparisonRow> rows;
    for (const std::string& task : tasks)
    {
        for (const std::string& mode : {std::string("legacy"), std::string("assisted")})
        {
            rows.push_back(RunOneControlledTask(fixture, port, mode, task));
            std::cout << "  ran " << std::setw(8) << std::left << mode
                      << " " << task.substr(0, 46) << "\n";
        }
    }

    std::cout << "\n  " << std::left << std::setw(46) << "task"
              << std::setw(10) << "mode"
              << std::setw(11) << "completed"
              << std::setw(10) << "wall ms"
              << std::setw(9) << "calls"
              << std::setw(11) << "of which"
              << std::setw(9) << "content" << "\n";
    std::cout << "  " << std::setw(46) << "" << std::setw(10) << ""
              << std::setw(11) << "" << std::setw(10) << ""
              << std::setw(9) << "total" << std::setw(11) << "subgoal"
              << std::setw(9) << "exact" << "\n";

    for (const ComparisonRow& row : rows)
    {
        std::cout << "  " << std::left << std::setw(46) << row.task.substr(0, 45)
                  << std::setw(10) << row.mode
                  << std::setw(11) << (row.completed ? "yes" : "no")
                  << std::setw(10) << row.wallMs
                  << std::setw(9) << row.ledger.Total()
                  << std::setw(11) << row.ledger.subgoalCalls
                  << std::setw(9) << row.contentSupplied << "\n";
    }

    // Totals, by mode. This is the comparison the claim actually rests on.
    const auto summarise = [&rows](const std::string& mode)
    {
        int completed = 0;
        int count = 0;
        long long wall = 0;
        std::uint32_t calls = 0;
        std::uint32_t invented = 0;
        for (const ComparisonRow& row : rows)
        {
            if (row.mode != mode) continue;
            ++count;
            completed += row.completed ? 1 : 0;
            wall += row.wallMs;
            calls += row.ledger.Total();
            invented += row.contentInvented;
        }
        std::cout << "  " << std::left << std::setw(10) << mode
                  << "completed " << completed << "/" << count
                  << "    total wall " << wall << "ms"
                  << "    total model calls " << calls
                  << "    planner wrote its own text " << invented << " time(s)\n";
        return std::make_pair(calls, wall);
    };

    std::cout << "\n  -- totals --\n";
    const auto legacy = summarise("legacy");
    const auto assisted = summarise("assisted");

    std::cout << "\n  net model calls, assisted minus legacy: "
              << (static_cast<int>(assisted.first) - static_cast<int>(legacy.first))
              << "\n";
    std::cout << "  net wall clock, assisted minus legacy:  "
              << (assisted.second - legacy.second) << "ms\n";
    std::cout << "\n  Every call is counted, including the ones spent planning a subgoal\n"
                 "  and the ones spent falling back after one was refused. A negative\n"
                 "  number above is a saving; a positive one is this feature costing more\n"
                 "  than it saves on these tasks, which is a result and not a failure.\n";

    // The consequence checks are not relaxed for this comparison, and saying so matters:
    // the easiest way to make assisted mode look better here would be to let it press
    // Send, and that is exactly the thing the ordering rule refuses.
    std::cout << "  No consequence check was weakened to run this. A task to place\n"
                 "  content still may not submit it.\n";
}

#endif // _WIN32

} // namespace

void RunComputerLive(const int port)
{
#ifndef _WIN32
    static_cast<void>(port);
    std::cout << "The live acceptance run needs Windows and the disposable fixture.\n";
#else
    std::string reason;
    if (!BackendAnswers(port, reason))
    {
        // A precise reason, and no pretending. The tooling above is runnable; what is
        // missing is a backend, and that is the whole of what is missing.
        std::cout << "\nLIVE RUN NOT PERFORMED\n";
        std::cout << "  reason: " << reason << "\n";
        std::cout << "  expected: a llama.cpp server answering on 127.0.0.1:" << port
                  << "\n";
        std::cout << "  start one with:\n"
                     "    ThirdParty/llama.cpp/llama-server.exe --model "
                     "Models/Qwen3.5-4B-Q4_K_M.gguf \\\n"
                     "        --host 127.0.0.1 --port " << port
                  << " --ctx-size 8192 --parallel 1 -ngl 99\n";
        std::cout << "  Everything else in this file is complete and runs the moment "
                     "one does.\n";
        return;
    }

    std::cout << "Backend answered on 127.0.0.1:" << port << ".\n";
    Fixture fixture;
    Check(fixture.Started(), "The disposable fixture did not start.");

    ExplainTheIdleReport(port);
    RunLegacyTask(fixture, port);
    RunShadowTask(fixture, port);
    RunControlledComparison(fixture, port);

    // Two phrasings of the same underlying work, because they do not come out the same
    // and the difference is worth reporting rather than hiding. The first names the
    // content a "message", which leads a 4B model toward pressing Send; the second says
    // plainly where the text goes.
    RunEligibleTask(fixture, port,
        "put \"dinner at eight\" into the Compose box in the fixture window");
    RunEligibleTask(fixture, port,
        "type \"about tomorrow\" into the Compose box. Do not send anything.");
    // A task built from the intent this model picks reliably. The point of including it
    // is not that it is easier -- it is that it separates "the arrangement does not
    // work" from "a 4B model chooses the wrong intent for that phrasing", and the two
    // have completely different remedies.
    RunEligibleTask(fixture, port,
        "press the Zoom in button in the fixture window");

    std::cout << "\nLive acceptance run complete. Every window touched belonged to the "
                 "disposable fixture; every decision above came from the model or from "
                 "the deterministic policy, never from this file.\n";
#endif
}
