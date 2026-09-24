#include "reviaSessionTestAccess.h"

#include <chrono>
#include <memory>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// Recording a controlled domain, by actually driving it.
//
//   ReviaTests.exe --computer-collect [output-directory]
//
// The dataset this writes is not synthetic. Every row comes from a decision that was
// really made, about a window that really existed, executed through the real pipeline,
// and verified -- or not verified -- by the real check. That distinction matters more
// than the row count: a policy trained on invented observations has learned a
// generator, and the first real screen it sees is out of distribution.
//
// It is also not broad. Everything here happens in one disposable fixture, so what it
// supports is a claim about that fixture and nothing else. The artifact the training
// tool produces records exactly that scope, and the runtime refuses it outside it.
//
// Off by default and opt-in for the same reason the recorder is: it opens a capture
// session, and a capture session that opens itself is a keylogger with a configuration
// file. The only application it will ever name is the fixture.

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
    explicit Fixture(std::string layout = "a")
        : layoutName(std::move(layout))
    {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        const std::filesystem::path directory =
            std::filesystem::path(modulePath).parent_path();
        executable = directory / FixtureExecutable;
        logFile = std::filesystem::temp_directory_path() /
            ("revia-collect-" + NewActionId() + ".log");

        const std::wstring wideLayout(layoutName.begin(), layoutName.end());
        const std::wstring commandLine =
            L"\"" + executable.wstring() + L"\" --log \"" + logFile.wstring() +
            L"\" --layout " + wideLayout;
        std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
        mutableCommandLine.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        started = CreateProcessW(
            executable.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
            0, nullptr, nullptr, &startup, &process) != FALSE;
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
    [[nodiscard]] const std::string& Layout() const { return layoutName; }

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

    // Puts the fixture's fields back to empty between tasks, so each task starts from
    // the same place and a later one is not verified by an earlier one's leftovers.
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
    std::string layoutName = "a";
};

responseOutput Answer(const std::string& text)
{
    responseOutput output;
    output.bSuccess = true;
    output.response = text;
    return output;
}

responseOutput Refused(const std::string& reason)
{
    responseOutput output;
    output.bSuccess = false;
    output.reason = reason;
    return output;
}

std::string Subgoal(
    const std::string& intent,
    const std::string& name = {},
    const std::string& role = {},
    const std::string& payloadId = {},
    const std::string& container = {})
{
    nlohmann::json target = {{"application", FixtureApplication}};
    if (!name.empty()) target["name"] = name;
    if (!role.empty()) target["role"] = role;
    if (!container.empty()) target["container"] = container;
    nlohmann::json subgoal = {
        {"intent", intent},
        {"description", "a bounded step in the fixture"},
        {"target", std::move(target)}};
    if (!payloadId.empty()) subgoal["payload_id"] = payloadId;
    return subgoal.dump();
}

// One controlled task with a known outcome.
struct Task
{
    std::string title;
    // What Main would propose, in order.
    std::vector<std::string> subgoals;
    // Content the runtime holds for this task, when it needs any.
    std::string payload;
};

// The task set.
//
// Chosen to cover the decisions the routine policy actually makes, and to include the
// ones it should decline: a control that is not there, and a description that fits two
// things. A dataset of only successes teaches a policy that it is always right.
std::vector<Task> ControlledTasks()
{
    // Varied on the axes that actually distinguish one decision from another: how the
    // target is named, whether it is named at all, whether more than one thing fits,
    // and whether it is there.
    //
    // A set of only clean successes would train a policy that everything is findable,
    // and its held-out score would look excellent right up until it met a real window.
    // The refusals below are the more valuable half of the dataset: they are the rows
    // that teach when not to act, and they are why coverage is reported separately from
    // accuracy.
    return {
        // -- named targets, several wordings --
        {"press zoom in", {Subgoal("interact_with_control", "Zoom in", "button")}, ""},
        {"press save", {Subgoal("interact_with_control", "Save", "button")}, ""},
        {"press open preferences",
            {Subgoal("interact_with_control", "Open preferences", "button")}, ""},
        {"press rename file",
            {Subgoal("interact_with_control", "Rename file", "button")}, ""},
        {"press vanishing control",
            {Subgoal("interact_with_control", "Vanishing control", "button")}, ""},
        {"press open delete account",
            {Subgoal("interact_with_control", "Open delete account", "button")}, ""},

        // -- the same targets with no role given, which is a different feature vector --
        {"press zoom in, role unsaid",
            {Subgoal("interact_with_control", "Zoom in")}, ""},
        {"press save, role unsaid", {Subgoal("interact_with_control", "Save")}, ""},

        // -- unnamed fields, identified by the panel they sit in --
        {"fill the compose panel",
            {"__payload__:" + std::string("Compose")}, "dinner at eight"},
        {"fill the subject panel",
            {"__payload__:" + std::string("Subject")}, "about tomorrow"},

        // -- window focus --
        {"bring the fixture forward", {Subgoal("focus_window")}, ""},

        // -- read-only resolution --
        {"find the send button", {Subgoal("resolve_target", "Send", "button")}, ""},
        {"find the delete button", {Subgoal("resolve_target", "Delete", "button")}, ""},

        // -- targets that are not there --
        {"press a control that is not there",
            {Subgoal("interact_with_control", "Publish everywhere", "button")}, ""},
        {"press another control that is not there",
            {Subgoal("interact_with_control", "Archive", "button")}, ""},
        {"find a control that is not there",
            {Subgoal("resolve_target", "Unsubscribe", "button")}, ""},

        // -- a name that is a fragment of a real one, which a loose matcher gets wrong --
        {"press a partial name", {Subgoal("interact_with_control", "Open", "button")}, ""},
        {"press another partial name",
            {Subgoal("interact_with_control", "Delete permanently", "button")}, ""},

        // -- an unnamed field with no container to distinguish it: unresolvable --
        {"fill a bare field",
            {Subgoal("enter_payload", "Document", "edit")}, "should not be typed"},

        // -- a panel that does not exist --
        {"fill a panel that is not there",
            {Subgoal("enter_payload", "", "edit", "", "Attachments")}, "nor this"},

        // -- wrong role for a real name --
        {"press an edit as if it were a button",
            {Subgoal("interact_with_control", "Save", "edit")}, ""},
    };
}

struct CollectionSession
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;

    explicit CollectionSession(const std::filesystem::path& datasetRoot)
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
        ReviaSessionTestAccess::UseRealStepProvider(session);

        computerControlSettings settings;
        settings.providerMode = "assisted";
        // Structure only. The candidate names and what they afford are what a ranker
        // learns from; the text inside the boxes is not, and asking for it would be
        // asking for consent this collection does not need.
        settings.captureDepth = "structure";
        ReviaSessionTestAccess::SetComputerSettings(session, settings);
        // After the settings, not before: ApplySettings resolves the configured
        // datasetDirectory and would otherwise put the rows back where the setting
        // says rather than where this run was told to write them.
        ReviaSessionTestAccess::UseDatasetRoot(session, datasetRoot);
    }

    [[nodiscard]] ComputerTaskCoordinator& Tasks()
    {
        return ReviaSessionTestAccess::ComputerTasks(session);
    }
};

#endif // _WIN32

} // namespace

void RunComputerCollection(const std::string& outputDirectory)
{
#ifndef _WIN32
    static_cast<void>(outputDirectory);
    std::cout << "Collection needs Windows and the disposable fixture; nothing was run.\n";
#else
    const std::filesystem::path datasetRoot = outputDirectory.empty()
        ? std::filesystem::path("RuntimeData") / "ComputerExperience"
        : std::filesystem::path(outputDirectory);

    CollectionSession collection(datasetRoot);

    CaptureConsent consent;
    consent.application = FixtureApplication;
    // These are Revia's own decisions under a teacher's supervision, not a person's
    // demonstrations. Saying so is the difference between a dataset that records what
    // works and one that claims a human did it.
    consent.provenance = ExperienceProvenance::StudentAction;
    consent.depth = CaptureDepth::Structure;
    consent.maximumRecords = 500;
    Check(collection.Tasks().Recorder().Begin(consent),
        "The capture session did not open.");

    const std::vector<Task> tasks = ControlledTasks();
    // Every task under every arrangement of the window.
    //
    // The two layouts hold the same controls, with the same names and the same
    // behaviour, in different places and a different order. Recording both is what makes
    // it possible to hold out a complete layout later and ask whether the policy learned
    // the task or the furniture -- a question the first trained ranker, which put +5.59
    // on position, would have failed and which no split available at the time could ask.
    const std::vector<std::string> layouts{"a", "b"};
    std::cout << "Recording " << tasks.size() << " controlled tasks in "
              << layouts.size() << " layouts into " << datasetRoot.string() << "\n\n";

    std::uint32_t decisions = 0;
    std::uint32_t modelCalls = 0;
    std::uint32_t routine = 0;

    for (const std::string& layout : layouts)
    {
    Fixture fixture(layout);
    Check(fixture.Started(), "The disposable fixture did not start.");
    std::cout << "-- layout " << layout << " --\n";

    for (const Task& task : tasks)
    {
        fixture.Reset();
        fixture.Front();
        // Protocol metadata, stamped on every row this task writes. Not a label and not
        // a feature: it is what lets a split hold out a whole task or a whole layout
        // instead of a whole session, which every task appeared in.
        collection.Tasks().Recorder().SetProtocol(task.title, layout);

        // A payload subgoal needs its reference minted first, so the marker in the task
        // list is expanded here rather than in the table above.
        std::vector<std::string> subgoals;
        for (const std::string& entry : task.subgoals)
        {
            if (entry.rfind("__payload__:", 0) != 0)
            {
                subgoals.push_back(entry);
                continue;
            }
            // The panel the unnamed field sits in. These fields have no name at all,
            // so the container is the only thing that identifies them -- which is the
            // case ISSUE-REVIA-0070 opened up and the one worth having rows for.
            const std::string container = entry.substr(12);
            const PayloadReference reference =
                collection.Tasks().HoldPayload(task.payload, "message");
            subgoals.push_back(
                Subgoal("enter_payload", "", "edit", reference.id, container));
        }

        ReviaSessionTestAccess::ScriptComputerProviders(
            collection.session,
            [subgoals, cursor = std::make_shared<std::size_t>(0)](
                const std::string&, std::stop_token)
            {
                if (*cursor >= subgoals.size()) return Refused("nothing further");
                return Answer(subgoals[(*cursor)++]);
            },
            [](const std::string&, std::stop_token)
            {
                return Answer(nlohmann::json{{"finished", true}}.dump());
            });

        Goal goal;
        goal.id = NewGoalId();
        goal.title = task.title;
        goal.budget.maxActions = 6;
        goal.budget.maxIdenticalSteps = 2;
        goal.scope = NarrowScopeForGoal(collection.session.Capabilities());

        ReviaSessionTestAccess::BeginComputerTask(collection.session, goal.id);
        const Goal finished =
            ReviaSessionTestAccess::OperateGoal(collection.session, std::move(goal));
        const ComputerControllerStats stats = collection.Tasks().Stats();
        ReviaSessionTestAccess::CompleteComputerTask(collection.session, finished);

        decisions += stats.decisions;
        modelCalls += stats.modelCalls;
        routine += stats.routineDecisions;

        std::cout << "  " << task.title << "\n"
                  << "    steps=" << finished.steps.size()
                  << "  stop=" << ToString(finished.stopReason)
                  << "  decisions=" << stats.decisions
                  << "  routine=" << stats.routineDecisions
                  << "  model=" << stats.modelCalls << "\n";
        if (!finished.steps.empty() && !finished.steps[0].attempts.empty())
        {
            // What the evidence actually established, per step. Printed because the
            // difference between "verified" and "unknown" is the difference between a
            // row that can carry a label and a row that cannot, and a collection run
            // that does not say which it produced is a run nobody can size up.
            std::cout << "    evidence="
                      << ToString(finished.steps[0].postcondition.kind)
                      << " -> " << ToString(finished.steps[0].attempts[0].outcome)
                      << "\n";
        }
    }
    }
    collection.Tasks().Recorder().SetProtocol({}, {});

    const CaptureStatus status = collection.Tasks().Recorder().Status();
    const std::string sessionId = status.sessionId;
    collection.Tasks().Recorder().End();

    std::cout << "\nrows written:   " << status.recorded << "\n";
    std::cout << "rows refused:   " << status.refused << "\n";
    std::cout << "session:        " << sessionId << "\n";
    std::cout << "dataset file:   "
              << collection.Tasks().Recorder().SessionPath(sessionId).string() << "\n";
    std::cout << "\nacross all tasks: " << decisions << " decisions, " << routine
              << " routine, " << modelCalls << " reached a model\n";
    std::cout << "\nEvery row here is a real decision about a real window in the "
                 "disposable fixture. It is evidence about that fixture and nothing "
                 "else.\n";
#endif
}
