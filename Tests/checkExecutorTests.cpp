#include "reviaSessionTestAccess.h"
#include "conversationRuntimeTestAccess.h"
#include "Agents/checkExecutor.h"
#include "Agents/investigationAgent.h"

#include <chrono>
#include <fstream>
#include <iostream>

namespace
{
using revia::agents::CheckCommand;
using revia::agents::CheckKind;
using revia::agents::ConfinedCheckExecutor;
using revia::agents::ConfinedCheckSettings;
using revia::agents::ExecutedCheck;
using revia::runtime::ReviaSession;
using revia::tests::Check;
using Access = revia::runtime::ReviaSessionTestAccess;

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

const char* Python()
{
#ifdef _WIN32
    return "python";
#else
    return "python3";
#endif
}

CheckCommand PythonCommand(const std::string& name, const std::string& code, const int timeoutSeconds = 30)
{
    CheckCommand command;
    command.name = name;
    command.command = Python();
    command.arguments = {"-c", code};
    command.timeoutSeconds = timeoutSeconds;
    return command;
}

void TestNamingIsHowAConfiguredCommandIsChosen()
{
    std::vector<CheckCommand> commands = {PythonCommand("tests", "print(1)"), PythonCommand("build", "print(2)")};
    Check(ConfinedCheckExecutor::MatchCommand(commands, "run the tests again", "").has_value() &&
            ConfinedCheckExecutor::MatchCommand(commands, "run the tests again", "")->name == "tests",
        "The command named in the check was not chosen.");
    Check(ConfinedCheckExecutor::MatchCommand(commands, "rebuild it", "does the build pass?")->name == "build",
        "A command named in the question was not chosen.");
    Check(!ConfinedCheckExecutor::MatchCommand(commands, "run the testsuite", "").has_value(),
        "A partial word matched a command name.");
    Check(!ConfinedCheckExecutor::MatchCommand(commands, "run everything", "").has_value(),
        "With two commands and none named, one was picked anyway.");
    commands.pop_back();
    Check(ConfinedCheckExecutor::MatchCommand(commands, "run everything", "")->name == "tests",
        "With one command and none named, it was not the one.");
}

void TestNamedPathsStayUnderTheirRoots()
{
    revia::tests::ScopedTestDirectory directory;
    const std::filesystem::path workspace = std::filesystem::absolute(directory.root / "ws");
    std::filesystem::create_directories(workspace / "src");
    std::ofstream(workspace / "src" / "main.cpp") << "int main() {}\n";
    std::ofstream(workspace / "config.json") << "{}\n";
    std::ofstream(directory.root / "outside.txt") << "secret\n";

    const auto named = ConfinedCheckExecutor::NamedPaths(
        "read src/main.cpp and \"config.json\", then " + (directory.root / "outside.txt").string() +
            " and missing.txt and the word tests",
        {workspace});
    Check(named.size() == 2 && named[0] == (workspace / "src" / "main.cpp").lexically_normal() &&
            named[1] == (workspace / "config.json").lexically_normal(),
        "Named paths were not resolved under the workspace, or an outside one slipped in.");
    Check(ConfinedCheckExecutor::NamedPaths((workspace / "src" / "main.cpp").string(), {workspace}).size() == 1,
        "An absolute path under the workspace was not accepted.");
    Check(ConfinedCheckExecutor::NamedPaths((workspace / ".." / "outside.txt").string(), {workspace}).empty(),
        "A dotted path out of the workspace was accepted.");
#ifndef _WIN32
    std::error_code error;
    std::filesystem::create_directory_symlink(directory.root, workspace / "link", error);
    if (!error)
    {
        Check(ConfinedCheckExecutor::NamedPaths("link/outside.txt", {workspace}).empty(),
            "A link out of the workspace was followed.");
    }
#endif
}

void TestAConfiguredCommandRunsBoundedAndObserved()
{
    revia::tests::ScopedTestDirectory directory;
    ConfinedCheckSettings settings;
    settings.workspace = std::filesystem::absolute(directory.root);
    settings.commands = {
        PythonCommand("tests", "import os\nprint('cwd ok' if os.path.isdir('.') else 'no')\nprint('42 passed')"),
        PythonCommand("failing", "import sys\nprint('boom')\nsys.exit(3)"),
        PythonCommand("slow", "import time\nprint('started', flush=True)\ntime.sleep(30)", 1),
        PythonCommand("chatty", "print('x' * 200)\nfor i in range(400): print('line', i)")};
    settings.maximumOutputCharacters = 600;
    std::vector<std::string> reported;
    const ConfinedCheckExecutor executor(settings,
        [&](const std::string& phase, const std::string& message) { reported.push_back(phase + ": " + message); });
    Check(executor.Available(), "An executor with commands was not available.");
    Check(Contains(executor.Describe(), "tests (") && Contains(executor.Describe(), "can be read") &&
            Contains(executor.Describe(), "Nothing else"),
        "The description does not tell the model what it may ask for: " + executor.Describe());

    ExecutedCheck ran = executor.Execute(CheckKind::Tests, "run the tests", "");
    Check(ran.ran && Contains(ran.observed, "exited with code 0") && Contains(ran.observed, "42 passed") &&
            Contains(ran.observed, "cwd ok") && ran.limitations.empty(),
        "A passing command was not observed: " + ran.observed + " / " + ran.refusal);
    Check(!reported.empty() && Contains(reported.front(), "Running: tests"), "The run was not reported.");

    ran = executor.Execute(CheckKind::Tests, "run the failing one", "");
    Check(ran.ran && Contains(ran.observed, "exited with code 3") && Contains(ran.observed, "boom"),
        "A failing command's exit code was not observed: " + ran.observed);

    const auto started = std::chrono::steady_clock::now();
    ran = executor.Execute(CheckKind::Tests, "run the slow check", "");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    Check(ran.ran && Contains(ran.observed, "did not finish within 1 s") && Contains(ran.observed, "started") &&
            Contains(ran.limitations, "did not complete"),
        "A command past its time limit was not stopped and said so: " + ran.observed);
    Check(elapsed < std::chrono::seconds(8), "The time limit was not enforced promptly.");

    ran = executor.Execute(CheckKind::Tests, "run chatty", "");
    Check(ran.ran && Contains(ran.observed, "last part") && Contains(ran.observed, "line 399") &&
            !Contains(ran.observed, "xxxxxxxxxx") && Contains(ran.limitations, "last 600"),
        "Long output was not cut to its tail: " + ran.observed.substr(0, 200));

    ran = executor.Execute(CheckKind::Tests, "run the linter", "");
    Check(!ran.ran && Contains(ran.refusal, "names none"), "An unnamed command ran anyway: " + ran.refusal);

    std::stop_source stop;
    stop.request_stop();
    ran = executor.Execute(CheckKind::Tests, "run the slow check", "", stop.get_token());
    Check(!ran.ran && Contains(ran.refusal, "stopped"), "A stopped check counted as run.");
}

void TestFilesAreReadAndTheRestIsRefused()
{
    revia::tests::ScopedTestDirectory directory;
    const std::filesystem::path workspace = std::filesystem::absolute(directory.root / "ws");
    const std::filesystem::path logs = std::filesystem::absolute(directory.root / "logs");
    std::filesystem::create_directories(workspace / "src");
    std::filesystem::create_directories(logs);
    std::ofstream(workspace / "src" / "main.cpp") << "int main() { return 7; }\n";
    std::ofstream(logs / "revia.log") << "[Log] started\n";
    std::ofstream(directory.root / "outside.txt") << "secret\n";
    ConfinedCheckSettings settings;
    settings.workspace = workspace;
    settings.logDirectory = logs;
    settings.maximumOutputCharacters = 5000;
    const ConfinedCheckExecutor executor(settings);
    Check(executor.Available() && settings.commands.empty(),
        "A workspace with no commands should still offer file reads.");
    Check(!Contains(executor.Describe(), "run by name"), "Commands were offered with none configured.");

    ExecutedCheck read = executor.Execute(CheckKind::SourceCode, "read src/main.cpp", "what does main return?");
    Check(read.ran && Contains(read.observed, "return 7") && Contains(read.observed, "main.cpp ("),
        "A source file under the workspace was not read: " + read.observed + " / " + read.refusal);
    read = executor.Execute(CheckKind::FileOrApplicationState, "list src", "");
    Check(read.ran && Contains(read.observed, "(folder)") && Contains(read.observed, "main.cpp"),
        "A folder was not listed: " + read.observed);
    read = executor.Execute(CheckKind::LogsAndMeasurements, "read revia.log", "");
    Check(read.ran && Contains(read.observed, "[Log] started"), "Her log was not read: " + read.refusal);
    read = executor.Execute(CheckKind::ResolvedConfiguration, "read " + (directory.root / "outside.txt").string(), "");
    Check(!read.ran && Contains(read.refusal, "no existing file"), "A file outside the workspace was read.");
    read = executor.Execute(CheckKind::Tests, "run the tests", "");
    Check(!read.ran && Contains(read.refusal, "no check commands"), "Tests ran with nothing configured.");
    read = executor.Execute(CheckKind::Research, "look it up", "");
    Check(!read.ran && Contains(read.refusal, "lookup path"), "Research was treated as a check.");
    read = executor.Execute(CheckKind::Calculation, "compute it", "");
    Check(!read.ran && Contains(read.refusal, "calculator"), "A calculation was treated as a check.");

    // A loop that gets one of these outcomes records it as an observation; a refusal
    // becomes a blocked question. That mapping lives in the runner and is exercised here
    // through the envelope, which must tell the model what it may really ask for.
    revia::agents::RoundRequest request;
    request.round = 1;
    request.goal = "Why does the build fail?";
    const std::string envelope = revia::agents::InvestigationAgent::BuildRoundEnvelope(
        request, "posture", true, executor.Describe());
    Check(Contains(envelope, "the runtime performs for you") && Contains(envelope, workspace.string()),
        "The envelope does not describe the real checks.");
    Check(Contains(revia::agents::InvestigationAgent::BuildRoundEnvelope(request, "posture", false), "NO tools"),
        "Without an executor the envelope stopped saying so.");
}

void TestTheSessionWiresChecksFromItsSettings()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    appSettings& settings = Access::Settings(session);
    const std::filesystem::path workspace = std::filesystem::absolute(directory.root / "checks");
    std::filesystem::create_directories(workspace);
    settings.codingAgent.workspace = workspace.string();
    checkCommandSettings tests;
    tests.name = "tests";
    tests.command = Python();
    tests.arguments = {"-c", "print('ok')"};
    settings.codingAgent.checkCommands = {tests};
    const auto configured = Access::CheckSettings(session);
    Check(configured.workspace == workspace.lexically_normal() && configured.commands.size() == 1 &&
            configured.commands.front().name == "tests" && !configured.logDirectory.empty(),
        "The session did not build the check settings from its own.");
    Access::ConfigureChecks(session);
    revia::runtime::ConversationRuntime& runtime = Access::Conversation(session);
    Check(runtime.HasCheckExecutor() &&
            Contains(revia::runtime::ConversationRuntimeTestAccess::ChecksDescription(runtime), "tests ("),
        "The runtime was not handed the checks.");

    settings.codingAgent.checkCommands.clear();
    settings.codingAgent.workspace = (directory.root / "does-not-exist").string();
    Access::ConfigureChecks(session);
    Check(!runtime.HasCheckExecutor(), "With nothing to check the runtime still claimed an executor.");
}

} // namespace

void RunCheckExecutorTests()
{
    TestNamingIsHowAConfiguredCommandIsChosen();
    TestNamedPathsStayUnderTheirRoots();
    TestAConfiguredCommandRunsBoundedAndObserved();
    TestFilesAreReadAndTheRestIsRefused();
    TestTheSessionWiresChecksFromItsSettings();
    std::cout << "An investigation can run a configured check or read a workspace file, bounded "
        "and confined, and what it observes is the executor's, never the model's.\n";
}
