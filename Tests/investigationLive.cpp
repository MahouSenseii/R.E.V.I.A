#include "testSupport.h"

#include "Agents/investigation.h"
#include "Agents/investigationAgent.h"
#include "Core/messageRouter.h"
#include "Library/structLibrary.h"

#include <chrono>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>

// The model-backed run.
//
// The deterministic suite proves the loop behaves correctly *given* results. It cannot
// prove that a language model reliably conducts an investigation, and presenting a
// fixture as though it did would be the central dishonesty available here. This runs the
// real thing against a real local model and prints whatever actually happens, including
// when that is disappointing.
//
// Opt-in, and deliberately not part of the default suite: it needs a llama-server on the
// configured port, and a suite that fails because a server is not running teaches nobody
// anything.

namespace
{

using namespace revia::agents;

std::string Truncate(const std::string& value, const std::size_t limit)
{
    if (value.size() <= limit) return value;
    return value.substr(0, limit) + "...";
}

} // namespace


namespace
{

// A real check executor, for the harness.
//
// Bounded and read-only: it answers a question by searching this repository's own source
// for the terms in it and reporting what it found. That is a genuine observation -- the
// text exists or it does not -- which is what the loop needs in order to have anything to
// learn from.
//
// It is emphatically NOT the production path. Production checks must route through
// ActionRuntime, CapabilityPolicy, approval and audit, and that wiring does not exist
// yet. This exists so the loop can be exercised end to end with real evidence, and the
// report says so rather than implying the runtime has tools it does not have.
ExecutedCheck SearchRepository(
    const std::filesystem::path& root,
    const CheckKind kind,
    const std::string& description,
    const std::string& questionText)
{
    ExecutedCheck result;
    if (kind == CheckKind::ModelReasoning)
    {
        result.refusal = "reasoning needs no executor";
        return result;
    }

    // The words worth searching for: long enough to be distinctive, and drawn from the
    // question rather than from the model's description of what it would do.
    std::vector<std::string> terms;
    std::string current;
    for (const unsigned char character : questionText)
    {
        if (std::isalnum(character) != 0 || character == '_')
        {
            current.push_back(static_cast<char>(character));
        }
        else
        {
            if (current.size() >= 6) terms.push_back(current);
            current.clear();
        }
    }
    if (current.size() >= 6) terms.push_back(current);
    if (terms.empty())
    {
        result.refusal = "the question contained no searchable term";
        return result;
    }
    if (terms.size() > 4) terms.resize(4);

    std::size_t filesScanned = 0;
    std::vector<std::string> hits;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(root, error), end;
         it != end && filesScanned < 400; it.increment(error))
    {
        if (error) break;
        const std::filesystem::path& path = it->path();
        const std::string name = path.filename().string();
        if (it->is_directory(error))
        {
            // Build output and third-party trees would swamp the result without adding
            // anything the question is about.
            if (name == "build" || name == "ThirdParty" || name == ".git" ||
                name == "Models" || name == "cmake-build-debug")
            {
                it.disable_recursion_pending();
            }
            continue;
        }
        const std::string extension = path.extension().string();
        if (extension != ".cpp" && extension != ".h" && extension != ".json") continue;

        std::ifstream file(path);
        if (!file) continue;
        ++filesScanned;
        std::string line;
        std::size_t lineNumber = 0;
        while (std::getline(file, line) && hits.size() < 6)
        {
            ++lineNumber;
            for (const std::string& term : terms)
            {
                if (line.find(term) == std::string::npos) continue;
                hits.push_back(name + ":" + std::to_string(lineNumber) + " " +
                    Truncate(line.substr(0, 120), 120));
                break;
            }
        }
        if (hits.size() >= 6) break;
    }

    result.ran = true;
    if (hits.empty())
    {
        result.observed = "Searched " + std::to_string(filesScanned) +
            " source files for " + terms.front() +
            " and found no occurrence.";
        result.limitations = "Only .cpp, .h and .json under the repository were searched.";
        return result;
    }
    std::ostringstream observed;
    observed << "Found " << hits.size() << " occurrence(s) while searching "
             << filesScanned << " files: ";
    for (std::size_t index = 0; index < hits.size() && index < 3; ++index)
    {
        observed << hits[index] << "; ";
    }
    result.observed = observed.str();
    result.limitations = "A textual match only -- it shows the text is present, not that "
        "it is what runs.";
    return result;
}

} // namespace

void RunInvestigationLive(const std::string& host, const int port)
{
    std::cout << "\n=== Live investigation (real model, no fixtures) ===\n";

    llmSettings llm;
    llm.backend = "LLamaCpp";
    llm.host = host;
    llm.port = port;
    llm.bAutoStartServer = false;
    // No multimodal projector is loaded for this harness, so vision is off. With it on
    // the health check correctly refuses a server that cannot do what was configured.
    llm.bVisionEnabled = false;
    // The health check verifies that the model the server reports is the one configured,
    // so this must be the path llama-server was launched with rather than a placeholder.
    llm.modelName =
        "C:/Users/davis/OneDrive/Documents/GitHub/R.E.V.I.A/Models/Qwen3.5-4B-Q4_K_M.gguf";

    const std::string posture =
        "You are Revia. You are working something out for yourself before answering. "
        "Be concrete and brief.";

    aiProfile profile;
    profile.displayName = "Revia";
    profile.systemPrompt = posture;

    messageRouter router;
    router.ApplyLLMSettings(llm, embeddingSettings{}, profile);

    // Reported rather than guessed at. A health check that fails silently sends you
    // hunting through the wrong layer.
    const healthOutput health = router.CheckLLMHealth();
    std::cout << "  Backend health : "
              << (health.bIsAvailable ? "available" : "UNAVAILABLE") << " -- "
              << health.message << " " << health.reason << "\n";

    // A goal with genuine internal structure, so a second round has something to be
    // *about*. A question answerable in one line would prove nothing about iteration.
    const std::string goal =
        "In the Revia C++ codebase, does the self-inquiry feature record a cooldown when "
        "a deliberation fails and produces no questions? Work out what the code actually "
        "does.";

    Investigation investigation(1, goal, "A cause narrowed to one class, with a "
        "distinguishing check named.");

    // Round one is the opening question set, exactly as the single self-inquiry pass
    // produces it.
    const auto openingStarted = std::chrono::steady_clock::now();
    const responseOutput opening = router.Deliberate(
        InvestigationAgent::BuildOpeningEnvelope(goal, posture, {}, 3), {});
    const double openingMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - openingStarted).count();

    if (!opening.bSuccess)
    {
        std::cout << "  The model was not reachable at " << host << ":" << port
                  << " -- " << opening.reason << "\n"
                  << "  No live result is claimed.\n";
        return;
    }

    const std::vector<ProposedQuestion> openingQuestions =
        InvestigationAgent::ParseOpeningQuestions(opening.response, 3);
    std::cout << "  Opening call: " << std::fixed << std::setprecision(0) << openingMs
              << " ms, " << openingQuestions.size() << " question(s) parsed\n";
    if (openingQuestions.empty())
    {
        std::cout << "  The model produced nothing parseable. Raw reply:\n    "
                  << Truncate(opening.response, 400) << "\n";
        return;
    }
    for (const ProposedQuestion& question : openingQuestions)
    {
        investigation.AddQuestion(question.text, question.materiality, 0);
    }

    std::cout << "\n  Revia is checking... (round 1)\n";
    for (const InvestigationQuestion& question : investigation.Questions())
    {
        std::cout << "    - " << question.text << "\n";
    }

    InvestigationBudget budget;
    budget.maximumRounds = 3;
    budget.maximumQuestionsPerRound = 2;
    budget.wallClock = std::chrono::milliseconds(120000);
    const InvestigationLoop loop(budget);

    // Recorded so the report can say whether a later round actually used an earlier
    // result, rather than asserting it.
    std::vector<std::size_t> priorFindingsSeen;
    const std::filesystem::path repository =
        "C:/Users/davis/OneDrive/Documents/GitHub/R.E.V.I.A";
    const CheckExecutor executor =
        [&repository](const CheckKind kind, const std::string& description,
            const std::string& questionText)
    {
        return SearchRepository(repository, kind, description, questionText);
    };
    const RoundRunner instrumented =
        [&](const RoundRequest& request) -> RoundResult
    {
        priorFindingsSeen.push_back(request.priorFindings.size());
        const RoundRunner real =
            InvestigationAgent::MakeRunner(router, posture, executor, {});
        return real(request);
    };

    const auto started = std::chrono::steady_clock::now();
    const RoundObserver observer = [](const RoundReport& round)
    {
        if (round.phase == RoundPhase::Checking)
        {
            if (round.round > 1)
            {
                std::cout << "\n  Revia is checking... (round " << round.round << ")\n";
                for (const std::string& question : round.questionsChecked)
                {
                    std::cout << "    - " << question << "\n";
                }
            }
            return;
        }
        std::cout << "  Findings (round " << round.round << ")\n";
        if (round.findingsSummary.empty())
        {
            std::cout << "    (nothing established this round)\n";
        }
        std::istringstream lines(round.findingsSummary);
        std::string line;
        while (std::getline(lines, line)) std::cout << "    " << line << "\n";
    };

    const InvestigationRunReport report =
        loop.Run(investigation, instrumented, {}, observer);
    const double totalMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();

    std::cout << "\n  Outcome        : " << ToString(report.outcome) << "\n";
    std::cout << "  Reason         : " << report.reason << "\n";
    std::cout << "  Rounds         : " << report.rounds << " (plus the opening call)\n";
    std::cout << "  Loop latency   : " << std::setprecision(0) << totalMs << " ms\n";
    std::cout << "  With opening   : " << (totalMs + openingMs) << " ms\n";
    std::cout << "  Approx tokens  : " << report.tokensUsed << "\n";
    std::cout << "  Tool calls     : " << report.toolCallsUsed
              << "  (harness executor: bounded read-only source search)\n";
    std::cout << "  Observations   : " << investigation.ObservationCount()
              << "  (tool observations, not interpretation)\n";
    if (report.completionRefused)
    {
        std::cout << "  Runtime refused an early completion: "
                  << report.completionRefusalReason << "\n";
    }

    std::cout << "\n  Did a later round receive earlier findings?\n";
    for (std::size_t index = 0; index < priorFindingsSeen.size(); ++index)
    {
        std::cout << "    round " << (index + 1) << " was handed "
                  << priorFindingsSeen[index] << " prior finding(s)\n";
    }

    std::cout << "\n  Questions, in the order they were raised:\n";
    for (const InvestigationQuestion& question : investigation.Questions())
    {
        std::cout << "    [" << ToString(question.status) << "] (raised round "
                  << question.raisedInRound << ") " << question.text << "\n";
    }
    std::cout << "\n  Findings:\n";
    for (const Finding& finding : investigation.Findings())
    {
        std::cout << "    [" << ToString(finding.kind) << "] round " << finding.round
                  << ": " << Truncate(finding.observed, 200) << "\n";
    }
    if (investigation.Findings().empty())
    {
        std::cout << "    (none)\n";
    }
}
