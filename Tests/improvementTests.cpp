#include "testSupport.h"

#include "Improvement/codeProposal.h"
#include "Improvement/improvementAgent.h"
#include "Improvement/proposalStore.h"
#include "Improvement/sourceCatalog.h"
#include "Improvement/workbench.h"
#include "Actions/actionTypes.h"
#include "LLM/LLamaCPP/llamaCppService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <string>
#include <vector>

// Revia reviewing her own code. Nothing here runs a model or a real build: the model is a
// scripted reply and the workbench build is a function that inspects the copy it was
// handed. What is tested is everything around them -- that a suggestion is checked before
// anything compiles it, that the real source is never written, that a proof compares
// against the unchanged code, and that verdicts come back as lessons.
namespace
{

using namespace revia::improvement;
using revia::tests::Check;
using json = nlohmann::json;

void Write(const std::filesystem::path& path, const std::string& content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
}

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

const std::string SpeechCode =
    "#include \"Speech/qwenTtsPool.h\"\n"
    "\n"
    "int Pick(int a, int b)\n"
    "{\n"
    "    // qwen_synthesis is timed here.\n"
    "    const char* metric = \"qwen_synthesis\";\n"
    "    (void)metric;\n"
    "    return a < b ? b : a;\n"
    "}\n"
    "\n"
    "int Twice(int a)\n"
    "{\n"
    "    return a + a;\n"
    "}\n";

// A small repository shaped like hers.
struct SourceTree
{
    revia::tests::ScopedTestDirectory directory;
    std::filesystem::path root = directory.root / "repo";

    SourceTree()
    {
        Write(root / "CMakeLists.txt", "project(Fixture)\n");
        Write(root / "CMakePresets.json", "{}\n");
        Write(root / "Private/Speech/qwenTtsPool.cpp", SpeechCode);
        Write(root / "Public/Speech/qwenTtsPool.h", "#pragma once\nint Pick(int a, int b);\n");
        Write(root / "Private/Resources/resourcePlanner.cpp", "int Plan() { return 1; }\n");
        Write(root / "Tests/fixtureTests.cpp", "int main() { return 0; }\n");
        Write(root / "Tools/qwen_tts_service.py", "print('voice')\n");
        Write(root / "Tools/Build.ps1", "Write-Output build\n");
        Write(root / "Config/settings.json", "{}\n");
        Write(root / "build/debug/libReviaFoundation.a", "not source\n");
        Write(root / "ThirdParty/big/model.bin", "weights\n");
    }
};

void TestSuggestionsAreParsedAndCheckedBeforeAnythingCompiles()
{
    std::string note;
    Check(!ParseReviewReply(R"({"found":false,"reason":"The code is fine."})", note) &&
        note == "The code is fine.", "A review that found nothing was not read as nothing.");
    Check(!ParseReviewReply("not json", note) && !note.empty(),
        "A malformed review was not refused with a reason.");

    const std::string reply = json{
        {"found", true}, {"title", "Pick the larger value"}, {"problem", "Returns the wrong one."},
        {"reason", "The comparison is inverted."}, {"file", "Private\\Speech\\qwenTtsPool.cpp"},
        {"find", "```cpp\n    return a < b ? b : a;\n```"}, {"replace", "    return a < b ? a : b;\n"},
        {"benefit", 1.7}, {"risk", -2.0}}.dump();
    const std::optional<CodeProposal> proposal = ParseReviewReply(reply, note);
    Check(proposal.has_value(), "A well-formed proposal was refused: " + note);
    Check(proposal->change.path == "Private/Speech/qwenTtsPool.cpp",
        "A backslashed path from the model was not normalised.");
    Check(proposal->change.find == "    return a < b ? b : a;\n",
        "A code fence around the snippet was kept: " + proposal->change.find);
    Check(proposal->benefit == 1.0 && proposal->risk == 0.0, "Scores were not held to 0..1.");
    Check(!proposal->fingerprint.empty(), "A proposal had no fingerprint to deduplicate by.");
    Check(!ParseReviewReply(R"({"found":true,"title":"x","problem":"y","reason":"z","file":"a.cpp"})",
        note), "A proposal with no code to change was accepted.");

    const auto check = [](const std::string& find, const std::string& replace)
    {
        return CheckChange({"Private/Speech/qwenTtsPool.cpp", find, replace}, SpeechCode);
    };
    Check(check("    return a < b ? b : a;\n", "    return a < b ? a : b;\n").ok,
        "A small, unique edit was refused.");
    Check(!check("{\n", "{ // brace\n").ok, "An edit matching more than one place was accepted.");
    Check(!check("    return nothing;\n", "    return 0;\n").ok,
        "An edit to code that is not there was accepted.");
    Check(!check("    return a < b ? b : a;\n", "    return a < b ? b : a;  \n").ok,
        "A change that changes nothing was accepted.");
    std::string huge;
    for (int index = 0; index < 70; ++index) huge += "    ++a;\n";
    Check(!check("    return a + a;\n", huge).ok, "An oversized change was accepted.");
    const ChangeCheck launch = check("    return a + a;\n",
        "    CreateProcessW(nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, nullptr, nullptr, nullptr);\n"
        "    return a + a;\n");
    Check(!launch.ok && launch.reason.find("CreateProcess") != std::string::npos,
        "A change that adds a process launch was accepted.");
    Check(!check("    return a + a;\n", "    system(\"del *\");\n    return a + a;\n").ok,
        "A change that adds a shell command was accepted.");

    // Existing code that already reaches outside may still be edited, as long as the
    // change does not add a new way to.
    const std::string launcher = "void Start()\n{\n    CreateProcessW(nullptr, nullptr);\n    int tries = 1;\n}\n";
    Check(CheckChange({"Private/Speech/x.cpp", "    CreateProcessW(nullptr, nullptr);\n    int tries = 1;\n",
            "    CreateProcessW(nullptr, nullptr);\n    int tries = 3;\n"}, launcher).ok,
        "Editing code that already launched a process was refused.");
    // ...but one launch in the edited lines does not license a second.
    Check(!CheckChange({"Private/Speech/x.cpp", "    CreateProcessW(nullptr, nullptr);\n    int tries = 1;\n",
            "    CreateProcessW(nullptr, nullptr);\n    CreateProcessW(nullptr, nullptr);\n    int tries = 1;\n"},
            launcher).ok,
        "A change that adds a second process launch beside an existing one was accepted.");
    // Spacing before the parenthesis is still a call.
    Check(!check("    return a + a;\n", "    std::system (\"del *\");\n    return a + a;\n").ok,
        "A shell command spelled with a space before its parenthesis was accepted.");

    // A model drops trailing spaces; that alone must not lose the edit.
    const std::string trailing = "int F()\n{\n    return 1;   \n}\n";
    Check(CheckChange({"Private/x.cpp", "    return 1;\n", "    return 2;\n"}, trailing).ok,
        "Trailing whitespace the model dropped made a real match fail.");
}

void TestABlockMissingItsSharedIndentStillLands()
{
    // Seen live: a block copied out of a function without the indentation every line of
    // it had. The shape is intact, so it is the same code, and the replacement is shifted
    // back to where it belongs.
    const std::string content =
        "void F()\n{\n    if (a)\n    {\n        Run();\n    }\n}\n";
    CodeChange change;
    change.path = "Private/Core/example.cpp";
    change.find = "if (a)\n{\n    Run();\n}\n";
    change.replace = "if (a && b)\n{\n    Run();\n}\n";
    Check(CheckChange(change, content).ok,
        "A block missing only its shared indentation was not found.");
    const std::optional<std::string> changed = ApplyChange(content, change);
    Check(changed && *changed ==
            "void F()\n{\n    if (a && b)\n    {\n        Run();\n    }\n}\n",
        "The replacement was not given back the indentation its block had.");

    // Flattened, the relative indentation is gone, and in Python that is different code.
    change.find = "if (a)\n{\nRun();\n}\n";
    Check(!CheckChange(change, content).ok,
        "A block with its relative indentation lost was accepted as the same code.");

    // When a copy misses, the line she invented is what gets reported.
    Check(FirstLineNotInFile(content, "if (a)\n{\n    Stop();\n}\n") == "Stop();",
        "The line that is not in the file was not the one reported.");
    Check(FirstLineNotInFile(content, change.find).empty(),
        "Real lines, merely out of shape, were reported as invented.");
}

void TestTheChangeAppliesAndTheDiffIsOneGitAccepts()
{
    const CodeChange change{"Private/Speech/qwenTtsPool.cpp",
        "    return a < b ? b : a;\n", "    return a < b ? a : b;"};
    const std::optional<std::string> changed = ApplyChange(SpeechCode, change);
    Check(changed.has_value() && changed->find("    return a < b ? a : b;\n}\n") != std::string::npos,
        "A replacement without its final newline joined two lines.");
    Check(!ApplyChange(SpeechCode, {"x", "{\n", "{"}).has_value(),
        "An ambiguous change was applied somewhere.");

    const std::string diff = MakeUnifiedDiff(SpeechCode, change);
    Check(diff.find("--- a/Private/Speech/qwenTtsPool.cpp") != std::string::npos &&
        diff.find("-    return a < b ? b : a;") != std::string::npos &&
        diff.find("+    return a < b ? a : b;") != std::string::npos,
        "The diff does not show the change: " + diff);

    // The patch is meant for `git apply` from the repository root, so apply it for real.
    revia::tests::ScopedTestDirectory directory;
    Write(directory.root / "Private/Speech/qwenTtsPool.cpp", SpeechCode);
    Write(directory.root / "change.patch", diff);
    // Line-ending conversion is the checkout's business, not the patch's.
    const std::string command = "git -c core.autocrlf=false -C \"" + directory.root.string() +
        "\" apply change.patch >nul 2>&1";
    if (std::system("git --version >nul 2>&1") == 0)
    {
        Check(std::system(command.c_str()) == 0, "git refused the generated patch:\n" + diff);
        Check(NormalizeLineEndings(ReadAll(directory.root / "Private/Speech/qwenTtsPool.cpp")) ==
            *changed,
            "Applying the patch did not produce the proposed file.");
    }

    CodeProposal proposal;
    proposal.id = "0001";
    proposal.title = "Pick the smaller value";
    proposal.problem = "It returns the larger.";
    proposal.reason = "The ternary is inverted.";
    proposal.change = change;
    const std::string markdown = ToMarkdown(proposal, diff);
    Check(markdown.find("## Why this change") != std::string::npos &&
        markdown.find("git apply 0001.patch") != std::string::npos,
        "The write-up lacks the reason or how to apply it.");
}

void TestOnlyProductCodeIsReviewable()
{
    for (const char* path : {"Private/Speech/speechService.cpp", "Public/Core/logger.h",
             "Desktop/reviaWindow.cpp", "Tools/qwen_tts_service.py"})
    {
        Check(SourceCatalog::IsReviewable(path), std::string("Product code was not reviewable: ") + path);
    }
    // The tests, the build, the scripts and the settings decide whether a change is
    // proven and what she may do, so a proposal may not touch them.
    for (const char* path : {"Tests/foundationTests.cpp", "CMakeLists.txt", "Tools/Build.ps1",
             "Tools/VerifyWorkbench.ps1", "Config/settings.json", "Tools/Presence/x.py",
             "../Private/a.cpp", "Private/../Tests/a.cpp", "Private\\a.cpp", "C:/Private/a.cpp",
             "/Private/a.cpp", "Private/a.txt", ""})
    {
        Check(!SourceCatalog::IsReviewable(path), std::string("This was reviewable: ") + path);
    }

    SourceTree tree;
    const auto located = SourceCatalog::LocateSourceRoot(tree.root / "build" / "debug");
    Check(located.has_value() && std::filesystem::equivalent(*located, tree.root),
        "The repository was not found from the build directory.");
    const SourceCatalog catalog(tree.root);
    const std::vector<std::string> files = catalog.Files();
    Check(std::find(files.begin(), files.end(), "Private/Speech/qwenTtsPool.cpp") != files.end() &&
        std::find(files.begin(), files.end(), "Tools/qwen_tts_service.py") != files.end(),
        "Reviewable files were not listed.");
    Check(std::none_of(files.begin(), files.end(), [](const std::string& file)
        {
            return file.rfind("Tests/", 0) == 0 || file.rfind("build/", 0) == 0 ||
                file == "Tools/Build.ps1";
        }), "Tests, build output, or scripts were listed as reviewable.");
    std::string content;
    std::string error;
    Check(!catalog.Read("Tests/fixtureTests.cpp", content, error), "A test file was readable for review.");
    // A file saved in a legacy code page cannot be sent to a model as JSON; it is refused
    // by name rather than failing the request that carries it.
    Write(tree.root / "Private/Core/legacy.cpp", "// caf\xE9 in Windows-1252\nint x = 1;\n");
    const bool legacyRead = catalog.Read("Private/Core/legacy.cpp", content, error);
    Check(!legacyRead && error.find("UTF-8") != std::string::npos,
        "A source file that is not UTF-8 was offered for review: " + error);

    const std::vector<std::string> voice = catalog.FilesFor({"Qwen3-TTS"}, {"qwen_synthesis"});
    Check(!voice.empty() && voice.front() == "Private/Speech/qwenTtsPool.cpp",
        "A component and its metric did not lead to the code that implements them.");
    const std::vector<std::string> named = catalog.Resolve("resourcePlanner");
    Check(!named.empty() && named.front() == "Private/Resources/resourcePlanner.cpp",
        "A file named in plain words was not found.");

    const CodeWindow window = ExtractWindow("x", SpeechCode, {"qwen_synthesis"}, 1, 4, 10000);
    Check(window.lastLine - window.firstLine + 1 <= 4 && window.text.find("qwen_synthesis") != std::string::npos,
        "The window was not bounded and centred on the metric.");
}

void TestProposalsAndVerdictsPersist()
{
    revia::tests::ScopedTestDirectory directory;
    std::string error;
    auto store = std::make_shared<ProposalStore>();
    Check(store->Initialize(directory.root / "Proposals", error), error);

    CodeProposal first;
    first.title = "First";
    first.change = {"Private/a.cpp", "int a = 1;\n", "int a = 2;\n"};
    first.status = ProposalStatus::Verified;
    Check(store->Save(first, "diff one", error) && first.id == "0001", "The first id was not 0001.");
    CodeProposal second;
    second.title = "Second";
    second.change = {"Private/b.cpp", "int b = 1;\n", "int b = 2;\n"};
    Check(store->Save(second, "diff two", error) && second.id == "0002", "Ids did not increase.");
    Check(store->Find("2").has_value() && store->Find("0002")->title == "Second",
        "A short id did not find its proposal.");
    Check(store->Known(Fingerprint(first.change)), "A saved change was not known.");
    Check(store->AwaitingDecision() == 1, "A proven proposal was not counted as waiting.");
    Check(std::filesystem::is_regular_file(store->PatchPath("1")) &&
        std::filesystem::is_regular_file(store->MarkdownPath("1")),
        "The patch or the write-up was not written.");

    Check(store->Decide("1", ProposalStatus::Rejected, "It hides a real error.", error), error);
    Check(!store->Decide("2", ProposalStatus::Verified, "", error),
        "A verdict other than accept or reject was allowed.");
    // A verdict that cannot be written is not held in memory either. A folder where the
    // pending record belongs makes the write fail on any platform and for any user.
    const std::filesystem::path blocked = directory.root / "Proposals" / "0002.json.pending";
    std::filesystem::create_directories(blocked);
    Check(!store->Decide("2", ProposalStatus::Accepted, "Looks right.", error),
        "A verdict that could not be written was reported as recorded.");
    Check(store->Find("2")->status == ProposalStatus::Drafted && store->Find("2")->feedback.empty(),
        "A verdict that never reached disk was kept in memory.");
    std::filesystem::remove_all(blocked);
    const std::vector<std::string> lessons = store->Lessons(5);
    Check(!lessons.empty() && lessons.front().find("REJECTED") != std::string::npos &&
        lessons.front().find("It hides a real error.") != std::string::npos,
        "A rejection's reason did not become a lesson.");

    store->MarkTaskReviewed("task-1");
    store->MarkExplored("Private/a.cpp", 221, 1000);

    ProposalStore reopened;
    Check(reopened.Initialize(directory.root / "Proposals", error), error);
    Check(reopened.All().size() == 2 && reopened.Find("1")->status == ProposalStatus::Rejected &&
        reopened.Find("1")->feedback == "It hides a real error.",
        "Proposals or verdicts were lost across a restart.");
    Check(reopened.TaskReviewed("task-1") && reopened.NextLine("Private/a.cpp") == 221 &&
        reopened.LastExplorationEpoch() == 1000, "Review progress was lost across a restart.");
    Check(reopened.LeastRecentlyExplored({"Private/a.cpp", "Private/b.cpp"}) == "Private/b.cpp",
        "Exploration did not move on to the file looked at longest ago.");
}

void TestTheWorkbenchProvesWithoutTouchingTheSource()
{
    SourceTree tree;
    const std::filesystem::path bench = tree.directory.root / "bench";
    std::atomic<int> runs = 0;
    std::string seenDuringRun;
    bool breakWhenPatched = false;
    bool alwaysFailing = false;
    bool buildFails = false;
    bool blockRestore = false;
    const BuildRunner runner = [&](const std::filesystem::path& source,
        const std::filesystem::path&, std::stop_token) -> BuildOutcome
    {
        ++runs;
        const std::filesystem::path proven = source / "Private/Speech/qwenTtsPool.cpp";
        seenDuringRun = ReadAll(proven);
        bool patched = seenDuringRun.find("a < b ? a : b") != std::string::npos;
        if (blockRestore && patched)
        {
            // Whatever holds the file outlives the build, so the revert cannot land.
            std::filesystem::remove(proven);
            std::filesystem::create_directory(proven);
        }
        // A copy that was never reverted is still the patched copy.
        patched = patched || (blockRestore && std::filesystem::is_directory(proven));
        BuildOutcome outcome;
        outcome.completed = true;
        if (buildFails)
        {
            outcome.buildLog = source.string() + "/Private/Speech/qwenTtsPool.cpp:8:5: error: 'x' was not declared\n";
            return outcome;
        }
        outcome.built = true;
        outcome.testsRan = true;
        const bool fail = (breakWhenPatched && patched) || alwaysFailing;
        outcome.testOutput =
            "  1/2 Test  #1: Revia.Foundation .................   Passed   74.90 sec\n"
            + std::string(fail ? "  2/2 Test  #2: Revia.OperatorSession ......***Failed    1.67 sec\n"
                               : "  2/2 Test  #2: Revia.OperatorSession ........   Passed    1.67 sec\n");
        return outcome;
    };
    Workbench workbench(tree.root, bench, runner);

    std::string error;
    const std::size_t copied = workbench.Sync(error);
    Check(error.empty() && copied > 0, "The first sync copied nothing: " + error);
    Check(std::filesystem::is_regular_file(workbench.MirrorRoot() / "Tests/fixtureTests.cpp") &&
        std::filesystem::is_regular_file(workbench.MirrorRoot() / "CMakeLists.txt"),
        "What the build needs did not reach the copy.");
    Check(!std::filesystem::exists(workbench.MirrorRoot() / "build") &&
        !std::filesystem::exists(workbench.MirrorRoot() / "ThirdParty"),
        "Build output or third-party files were copied.");
    Check(workbench.Sync(error) == 0, "An unchanged tree was copied again.");
    std::filesystem::remove(tree.root / "Private/Resources/resourcePlanner.cpp");
    Check(workbench.Sync(error) == 1 &&
        !std::filesystem::exists(workbench.MirrorRoot() / "Private/Resources/resourcePlanner.cpp"),
        "A file deleted from the source stayed in the copy, where the build would compile it.");

    const CodeChange change{"Private/Speech/qwenTtsPool.cpp",
        "    return a < b ? b : a;\n", "    return a < b ? a : b;\n"};
    VerificationResult result = workbench.Verify(change, {});
    Check(result.concluded && result.verified && result.summary.find("2/2") != std::string::npos,
        "A change that builds and passes was not proven: " + result.summary);
    Check(seenDuringRun.find("a < b ? a : b") != std::string::npos,
        "The build did not see the change it was proving.");
    Check(ReadAll(tree.root / "Private/Speech/qwenTtsPool.cpp") == SpeechCode,
        "Proving a change wrote to the real source.");
    Check(ReadAll(workbench.MirrorRoot() / "Private/Speech/qwenTtsPool.cpp") == SpeechCode,
        "The copy kept the change after the proof.");

    // A failure the change causes is caught by comparing against the unchanged copy.
    breakWhenPatched = true;
    runs = 0;
    result = workbench.Verify(change, {});
    Check(result.concluded && !result.verified && result.summary.find("Revia.OperatorSession") != std::string::npos &&
        runs == 2, "A change that broke a suite was not caught by the comparison: " + result.summary);

    // One that was already failing is not the change's fault.
    breakWhenPatched = false;
    alwaysFailing = true;
    std::filesystem::remove(bench / "manifest.json");
    Workbench fresh(tree.root, bench, runner);
    result = fresh.Verify(change, {});
    Check(result.concluded && result.verified && result.summary.find("already fail") != std::string::npos,
        "A suite that fails without the change blocked the proof: " + result.summary);

    alwaysFailing = false;
    buildFails = true;
    result = fresh.Verify(change, {});
    Check(result.concluded && !result.verified && result.buildErrors.find("error:") != std::string::npos &&
        result.buildErrors.find(fresh.MirrorRoot().string()) == std::string::npos,
        "A build failure did not return its errors for a repair, or leaked the workbench path.");
    buildFails = false;

    result = fresh.Verify({"Tests/fixtureTests.cpp", "int main() { return 0; }\n", "int main() { return 1; }\n"}, {});
    Check(result.concluded && !result.verified, "A change to the tests was proven.");

    // A revert that cannot land leaves the copy patched, and a comparison run on it would
    // fail the same way and excuse the change. No verdict until the copy is clean again.
    breakWhenPatched = true;
    blockRestore = true;
    result = fresh.Verify(change, {});
    Check(!result.concluded && !result.verified,
        "A proof whose revert failed was judged against the still-patched copy: " + result.summary);
    blockRestore = false;
    breakWhenPatched = false;
    std::filesystem::remove(fresh.MirrorRoot() / "Private/Speech/qwenTtsPool.cpp");
    Check(fresh.Sync(error) >= 1 &&
        ReadAll(fresh.MirrorRoot() / "Private/Speech/qwenTtsPool.cpp") == SpeechCode,
        "The next sync did not restore a file whose revert failed.");

    const std::vector<TestOutcome> parsed = ParseCtestOutput(
        "      Start  1: revia_web_guest\n"
        " 1/3 Test  #1: revia_web_guest ..................   Passed    2.06 sec\n"
        " 2/3 Test  #2: Revia.Foundation .................***Failed   74.90 sec\n"
        " 3/3 Test  #3: Revia.SvgRenderer ................***Timeout 900.00 sec\n");
    Check(parsed.size() == 3 && parsed[0].passed && !parsed[1].passed && !parsed[2].passed &&
        parsed[1].name == "Revia.Foundation", "CTest's summary lines were misread.");
}

struct AgentFixture
{
    SourceTree tree;
    std::shared_ptr<ProposalStore> store = std::make_shared<ProposalStore>();
    std::vector<std::string> replies;
    std::size_t calls = 0;
    std::vector<std::string> instructionsSeen;
    std::vector<std::string> reports;
    bool quiet = true;
    int buildsThatFail = 0;
    ImprovementAgent agent;

    AgentFixture()
    {
        std::string error;
        Check(store->Initialize(tree.directory.root / "Proposals", error), error);
        ImprovementAgent::Dependencies dependencies;
        dependencies.catalog = SourceCatalog(tree.root);
        dependencies.store = store;
        dependencies.workbench = std::make_shared<Workbench>(tree.root, tree.directory.root / "bench",
            [this](const std::filesystem::path& source, const std::filesystem::path&, std::stop_token)
            {
                BuildOutcome outcome;
                outcome.completed = true;
                if (buildsThatFail > 0)
                {
                    --buildsThatFail;
                    outcome.buildLog = source.string() + "/Private/Speech/qwenTtsPool.cpp:8: error: no member named 'x'\n";
                    return outcome;
                }
                outcome.built = true;
                outcome.testsRan = true;
                outcome.testOutput = "  1/1 Test  #1: Revia.Foundation ......   Passed   1.0 sec\n";
                return outcome;
            });
        dependencies.review = [this](const std::string& instructions, const std::string&,
            const std::string&, std::stop_token)
        {
            instructionsSeen.push_back(instructions);
            responseOutput output;
            output.bSuccess = true;
            output.response = calls < replies.size() ? replies[calls] : R"({"found":false,"reason":"done"})";
            ++calls;
            return output;
        };
        dependencies.idle = [this](int, bool) { return quiet; };
        dependencies.report = [this](const CodeProposal&, const std::string& message) { reports.push_back(message); };
        improvementSettings settings;
        agent.Configure(settings, std::move(dependencies));
    }

    static std::string Reply(const std::string& file, const std::string& find, const std::string& replace,
        const double benefit, const double risk = 0.1)
    {
        return json{{"found", true}, {"title", "Return the smaller value"},
            {"problem", "Pick returns the larger value."}, {"reason", "The ternary is inverted."},
            {"evidence", "Line 8."}, {"file", file}, {"find", find}, {"replace", replace},
            {"benefit", benefit}, {"risk", risk}}.dump();
    }

    ReviewJob Job(const std::string& trigger) const
    {
        ReviewJob job;
        job.trigger = trigger;
        job.file = "Private/Speech/qwenTtsPool.cpp";
        job.problem = "Voice phrase generation repeatedly exceeds 12 seconds.";
        job.evidence = "6 phrases crossed the stall threshold.";
        job.anchors = {"qwen_synthesis"};
        return job;
    }
};

void TestSheProposesProvesAndReports()
{
    AgentFixture fixture;
    const std::string fix = AgentFixture::Reply("Private/Speech/qwenTtsPool.cpp",
        "    return a < b ? b : a;\n", "    return a < b ? a : b;\n", 0.7);
    fixture.replies = {fix, fix};

    ReviewOutcome outcome = fixture.agent.Run(fixture.Job("exploration"), {});
    Check(outcome.kind == ReviewOutcome::Kind::Proposed && outcome.proposal &&
        outcome.proposal->status == ProposalStatus::Verified,
        "A sound suggestion was not proposed and proven: " + outcome.note);
    Check(fixture.reports.size() == 1 && fixture.reports.front().find("/improve show 0001") != std::string::npos,
        "The proven proposal was not reported with how to see it.");
    Check(ReadAll(fixture.tree.root / "Private/Speech/qwenTtsPool.cpp") == SpeechCode,
        "Reviewing and proving changed her real source.");

    outcome = fixture.agent.Run(fixture.Job("exploration"), {});
    Check(outcome.kind == ReviewOutcome::Kind::Refused && outcome.note.find("before") != std::string::npos,
        "The same change was proposed twice.");
    Check(fixture.reports.size() == 1, "A refused suggestion was reported.");
}

void TestTheBarDependsOnWhySheWasLooking()
{
    AgentFixture fixture;
    const std::string modest = AgentFixture::Reply("Private/Speech/qwenTtsPool.cpp",
        "    return a + a;\n", "    return a * 2;\n", 0.45);
    fixture.replies = {modest, modest};
    // Found on her own, a modest change is not worth anyone's time.
    ReviewOutcome outcome = fixture.agent.Run(fixture.Job("exploration"), {});
    Check(outcome.kind == ReviewOutcome::Kind::Refused && outcome.note.find("worth") != std::string::npos,
        "A modest idea found while idle cleared the high bar: " + outcome.note);
    // Aimed at a measured problem, the same estimate clears the lower bar.
    outcome = fixture.agent.Run(fixture.Job("evidence"), {});
    Check(outcome.kind == ReviewOutcome::Kind::Proposed,
        "A change aimed at a measured problem was held to the idle bar: " + outcome.note);
    Check(fixture.instructionsSeen.back().find("Voice phrase generation repeatedly exceeds") != std::string::npos,
        "The measured problem was not in front of her when she reviewed.");

    const std::string risky = AgentFixture::Reply("Private/Speech/qwenTtsPool.cpp",
        "int Twice(int a)\n", "int Twice(long a)\n", 0.9, 0.8);
    fixture.replies.push_back(risky);
    outcome = fixture.agent.Run(fixture.Job("evidence"), {});
    Check(outcome.kind == ReviewOutcome::Kind::Refused, "A change she rated risky was kept.");
}

void TestSuggestionsSheCannotBackAreSetAside()
{
    AgentFixture fixture;
    fixture.replies = {
        R"({"found":false,"reason":"Nothing here is wrong."})",
        AgentFixture::Reply("Private/Resources/resourcePlanner.cpp", "int Plan() { return 1; }\n",
            "int Plan() { return 2; }\n", 0.9),
        AgentFixture::Reply("Private/Speech/qwenTtsPool.cpp", "    return a + a;\n",
            "    system(\"curl example.com\");\n    return a + a;\n", 0.9),
        AgentFixture::Reply("Private/Speech/qwenTtsPool.cpp", "    return a - a;\n", "    return 0;\n", 0.9)};
    Check(fixture.agent.Run(fixture.Job("exploration"), {}).kind == ReviewOutcome::Kind::Nothing,
        "Finding nothing was not the outcome when she found nothing.");
    Check(fixture.agent.Run(fixture.Job("exploration"), {}).kind == ReviewOutcome::Kind::Refused,
        "An edit to a file she was not shown was kept.");
    Check(fixture.agent.Run(fixture.Job("exploration"), {}).kind == ReviewOutcome::Kind::Refused,
        "An edit adding a shell command was kept.");
    Check(fixture.agent.Run(fixture.Job("exploration"), {}).kind == ReviewOutcome::Kind::Refused,
        "An edit to code that is not there was kept.");
    Check(fixture.store->All().empty() && fixture.reports.empty(), "A set-aside idea was recorded or reported.");
}

void TestAPathSpelledInAnotherCaseIsTheSameFile()
{
    // Seen live: LLamaCppServerProcess.cpp for llamaCppServerProcess.cpp, refused as a
    // file she was not shown. Windows does not care, so neither does the check.
    AgentFixture fixture;
    fixture.replies = {AgentFixture::Reply("private/speech/QwenTtsPool.cpp",
        "    return a < b ? b : a;\n", "    return a < b ? a : b;\n", 0.8)};
    const ReviewOutcome outcome = fixture.agent.Run(fixture.Job("request"), {});
    Check(outcome.kind == ReviewOutcome::Kind::Proposed && outcome.proposal &&
        outcome.proposal->change.path == "Private/Speech/qwenTtsPool.cpp",
        "A path that differs only in case was refused or kept its wrong spelling.");
}

void TestShortenedCodeGetsOneSecondLook()
{
    // Seen live: "// Clip lengths arrive in a header... so the body stays one" -- the
    // lines she meant, shortened with "...", which can never be found.
    AgentFixture fixture;
    fixture.replies = {
        AgentFixture::Reply("Private/Speech/qwenTtsPool.cpp", "    return a < b ... a;\n",
            "    return a < b ? a : b;\n", 0.8),
        AgentFixture::Reply("Private/Speech/qwenTtsPool.cpp", "    return a < b ? b : a;\n",
            "    return a < b ? a : b;\n", 0.8)};
    const ReviewOutcome outcome = fixture.agent.Run(fixture.Job("request"), {});
    Check(outcome.kind == ReviewOutcome::Kind::Proposed && outcome.proposal &&
        outcome.proposal->change.find == "    return a < b ? b : a;\n",
        "A change whose find was shortened was not corrected on a second look.");
    Check(fixture.instructionsSeen.size() >= 2 &&
        fixture.instructionsSeen[1].find("no \"...\"") != std::string::npos &&
        fixture.instructionsSeen[1].find("return a < b ... a;") != std::string::npos,
        "The second look was not told what it wrote and what was wrong with it.");
}

void TestASecondIdeaAboutOpenCodeWaits()
{
    // Seen live: the same lines proposed again in other words while the first proposal
    // still waited for a decision.
    AgentFixture fixture;
    fixture.replies = {
        AgentFixture::Reply("Private/Speech/qwenTtsPool.cpp", "    return a < b ? b : a;\n",
            "    return a < b ? a : b;\n", 0.8),
        AgentFixture::Reply("Private/Speech/qwenTtsPool.cpp", "    return a < b ? b : a;\n",
            "    return std::min(a, b);\n", 0.8)};
    const ReviewOutcome first = fixture.agent.Run(fixture.Job("request"), {});
    const ReviewOutcome second = fixture.agent.Run(fixture.Job("request"), {});
    Check(first.kind == ReviewOutcome::Kind::Proposed && first.proposal &&
        second.kind == ReviewOutcome::Kind::Refused &&
        second.note.find("#" + first.proposal->id) != std::string::npos,
        "A second change to code with a proposal still open was not held back.");
}

void TestOneRepairAfterABuildError()
{
    AgentFixture fixture;
    fixture.buildsThatFail = 1;
    fixture.replies = {
        AgentFixture::Reply("Private/Speech/qwenTtsPool.cpp", "    return a < b ? b : a;\n",
            "    return x < b ? a : b;\n", 0.8),
        AgentFixture::Reply("Private/Speech/qwenTtsPool.cpp", "    return a < b ? b : a;\n",
            "    return a < b ? a : b;\n", 0.8)};
    const ReviewOutcome outcome = fixture.agent.Run(fixture.Job("request"), {});
    Check(outcome.proposal && outcome.proposal->status == ProposalStatus::Verified &&
        outcome.proposal->verificationSummary.find("repair") != std::string::npos &&
        outcome.proposal->change.replace.find("a < b ? a : b") != std::string::npos,
        "A change that failed to build was not repaired from the compiler's errors.");
    Check(fixture.instructionsSeen.size() == 2 &&
        fixture.instructionsSeen.back().find("no member named 'x'") != std::string::npos,
        "The repair was not shown the compiler's own words.");
}

void TestABuildWaitsForQuietAndVerdictsBecomeLessons()
{
    AgentFixture fixture;
    fixture.quiet = false;
    fixture.replies = {AgentFixture::Reply("Private/Speech/qwenTtsPool.cpp",
        "    return a < b ? b : a;\n", "    return a < b ? a : b;\n", 0.8)};
    const ReviewOutcome outcome = fixture.agent.Run(fixture.Job("evidence"), {});
    Check(outcome.proposal && outcome.proposal->status == ProposalStatus::Drafted &&
        outcome.proposal->verificationSummary.find("quiet") != std::string::npos,
        "A build she decided on alone ran while someone was using her.");
    Check(fixture.reports.empty(), "An unproven proposal was reported as found.");

    std::string error;
    Check(fixture.store->Decide(outcome.proposal->id, ProposalStatus::Rejected,
        "Swapping the ternary changes the API contract.", error), error);
    fixture.quiet = true;
    (void)fixture.agent.Run(fixture.Job("exploration"), {});
    Check(fixture.instructionsSeen.back().find("Swapping the ternary changes the API contract.") != std::string::npos,
        "A rejection's reason was not in front of her at the next review.");
    Check(fixture.instructionsSeen.back().find("Quentin") == std::string::npos,
        "The review prompt names a particular person.");
}

} // namespace

void TestAFailedReviewDoesNotEndHerProcess()
{
    // Everything around the review runs on the agent's own thread. An exception thrown
    // there -- a request that could not be encoded, say -- used to escape the thread,
    // and an exception escaping a std::jthread ends the process.
    AgentFixture fixture;
    std::atomic<int> reviews = 0;
    ImprovementAgent::Dependencies dependencies;
    dependencies.catalog = SourceCatalog(fixture.tree.root);
    dependencies.store = fixture.store;
    dependencies.review = [&reviews](const std::string&, const std::string&,
        const std::string&, std::stop_token) -> responseOutput
    {
        if (reviews.fetch_add(1) == 0) throw std::runtime_error("the request could not be encoded");
        responseOutput output;
        output.bSuccess = true;
        output.response = R"({"found":false,"reason":"nothing to change"})";
        return output;
    };
    fixture.agent.Configure(improvementSettings{}, std::move(dependencies));
    fixture.agent.Start();
    const auto waitFor = [&reviews](const int count)
    {
        for (int tick = 0; tick < 500 && reviews.load() < count; ++tick)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return reviews.load() >= count;
    };
    std::string message;
    Check(fixture.agent.Request("Private/Speech/qwenTtsPool.cpp", message) && waitFor(1),
        "The requested review never ran: " + message);
    Check(fixture.agent.Request("Private/Speech/qwenTtsPool.cpp", message) && waitFor(2),
        "A review that threw stopped her from reviewing anything else.");
    Check(fixture.agent.Running(), "The review thread ended after a failed review.");
    fixture.agent.Stop();
}

void TestMalformedTextIsReportedNotThrown()
{
    // The planner path, which a review goes through, encodes its request as JSON. Text
    // that is not UTF-8 used to throw out of it; now it is a failed request with a reason,
    // decided before any connection is attempted.
    const llamaCppService service;
    responseOutput output;
    bool threw = false;
    try
    {
        output = service.GenerateCodeReview("Review this code.", "// caf\xE9\nint x = 1;\n", "");
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    Check(!threw && !output.bSuccess && output.reason.find("could not be encoded") != std::string::npos,
        "Malformed text in a planner request escaped as an exception: " + output.reason);
}

void RunImprovementTests()
{
    TestSuggestionsAreParsedAndCheckedBeforeAnythingCompiles();
    TestTheChangeAppliesAndTheDiffIsOneGitAccepts();
    TestABlockMissingItsSharedIndentStillLands();
    TestOnlyProductCodeIsReviewable();
    TestProposalsAndVerdictsPersist();
    TestTheWorkbenchProvesWithoutTouchingTheSource();
    TestSheProposesProvesAndReports();
    TestTheBarDependsOnWhySheWasLooking();
    TestSuggestionsSheCannotBackAreSetAside();
    TestAPathSpelledInAnotherCaseIsTheSameFile();
    TestShortenedCodeGetsOneSecondLook();
    TestASecondIdeaAboutOpenCodeWaits();
    TestOneRepairAfterABuildError();
    TestABuildWaitsForQuietAndVerdictsBecomeLessons();
    TestAFailedReviewDoesNotEndHerProcess();
    TestMalformedTextIsReportedNotThrown();
    std::cout << "Self-improvement tests passed: suggestions are checked, proven in a copy, "
                 "never written to her source, and verdicts come back as lessons.\n";
}

// Live only: proves a one-word comment edit through the real workbench -- the real sync,
// Tools/VerifyWorkbench.ps1, a full build of the copy, and every test suite. Run from the
// build directory; the first run builds the whole copy and takes a while.
int RunImprovementWorkbenchLive()
{
    using namespace revia::improvement;
    using revia::tests::Check;
    const auto root = SourceCatalog::LocateSourceRoot(std::filesystem::current_path());
    Check(root.has_value(), "No source tree above the current directory.");
    const char* local = std::getenv("LOCALAPPDATA");
    Check(local != nullptr, "LOCALAPPDATA is not set.");
    const std::filesystem::path bench = std::filesystem::path(local) / "Revia" / "ImprovementWorkbench";
    Workbench workbench(*root, bench, MakeScriptRunner(
        *root / "Tools" / "VerifyWorkbench.ps1",
        std::filesystem::current_path() / "_deps",
        bench / "logs",
        std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2),
        180));

    const SourceCatalog catalog(*root);
    std::string content;
    std::string error;
    Check(catalog.Read("Private/Core/logger.cpp", content, error), error);
    std::istringstream lines(content);
    std::string comment;
    for (std::string line; std::getline(lines, line);)
    {
        const std::size_t text = line.find_first_not_of(' ');
        if (text != std::string::npos && line.compare(text, 3, "// ") == 0 && line.size() > 20)
        {
            comment = line + "\n";
            break;
        }
    }
    Check(!comment.empty(), "No comment line to edit in logger.cpp.");
    std::string edited = comment;
    edited.insert(edited.size() - 1, " (workbench check)");
    const CodeChange change{"Private/Core/logger.cpp", comment, edited};
    Check(CheckChange(change, content).ok, "The live check's own edit failed the checks.");

    std::cout << "Proving a comment edit in " << revia::actions::PathToUtf8(workbench.Root()) << "...\n";
    const VerificationResult result = workbench.Verify(change, {});
    std::cout << "Result after " << static_cast<int>(result.seconds) << " s: " << result.summary << "\n";
    if (!result.buildErrors.empty()) std::cout << result.buildErrors << "\n";
    std::string after;
    Check(catalog.Read("Private/Core/logger.cpp", after, error) && after == content,
        "The live proof changed the real source.");
    Check(result.concluded && result.verified, "The workbench did not prove a harmless edit.");
    return 0;
}
