#include "testSupport.h"

#include "Computer/computerBenchmark.h"
#include "Computer/learnedPolicy.h"
#include "Computer/routinePolicy.h"

#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{

using namespace revia::computer;
using revia::tests::Check;

// A stand-in for the model-driven path, so the comparison has a baseline.
//
// It answers the way an unconstrained decision path plausibly would on these cases:
// it takes the first candidate whose name contains the one it was asked for. That is
// not a caricature -- substring matching is what a prompt-driven path does when the
// model reads a list and picks something that looks right -- and it is exactly the
// behaviour the near-miss family exists to catch.
//
// It is labelled as a stand-in everywhere it appears. The real Main path needs a model
// and cannot run in a headless suite, so what this measures is the *shape* of the
// difference, not Main's actual accuracy.
class SubstringBaselinePolicy final : public IComputerPolicy
{
public:
    explicit SubstringBaselinePolicy(const ComputerSubgoal*& current)
        : subgoal(&current)
    {
    }

    [[nodiscard]] std::string Name() const override { return "substring_baseline"; }
    [[nodiscard]] bool IsAvailable() const override { return true; }

    [[nodiscard]] ComputerDecision Decide(
        const ComputerTaskContext& context, std::stop_token) override
    {
        ComputerDecision decision;
        decision.provider = Name();
        // A model answered, so this is what a decision costs when it reaches one. The
        // figure is nominal; what it marks is that a call happened at all.
        decision.tokens = 400;
        decision.costReported = true;

        const ComputerSubgoal* active = *subgoal;
        if (active == nullptr || !context.observation.Available())
        {
            decision.detail = "Nothing to decide from.";
            return decision;
        }

        const std::string wanted = Lowered(active->target.name);
        for (const ObservedCandidate& candidate : context.observation.candidates)
        {
            if (wanted.empty()) break;
            if (Lowered(candidate.name).find(wanted) == std::string::npos) continue;

            decision.kind = ComputerDecisionKind::ProposeAction;
            decision.step.action.type =
                active->intent == SubgoalIntent::EnterPayload
                    ? revia::actions::ActionType::SetControlText
                    : revia::actions::ActionType::InvokeControl;
            decision.step.action.application = active->target.application;
            decision.step.action.control = candidate.id;
            decision.detail = "Use \"" + candidate.name + "\".";
            return decision;
        }

        decision.detail = "Nothing matched.";
        return decision;
    }

private:
    static std::string Lowered(std::string value)
    {
        for (char& character : value)
        {
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character)));
        }
        return value;
    }

    const ComputerSubgoal** subgoal = nullptr;
};

// The comparison the design asks for, run on the same cases for every provider.
void TestTheProvidersAreComparedOnTheSameProblems()
{
    // One vault, shared with every policy under test. See StandardBenchmarkCases.
    PayloadVault vault;
    const std::vector<BenchmarkCase> cases = StandardBenchmarkCases(vault);
    Check(!cases.empty(), "The standard benchmark has no cases.");
    for (const BenchmarkCase& testCase : cases)
    {
        Check(testCase.subgoal.Validated(),
            "Benchmark case \"" + testCase.name + "\" carries a subgoal the runtime "
            "would never produce, so it measures a problem no policy actually faces.");
    }

    BenchmarkReport report;
    report.repetitions = 20;

    // The deterministic policy.
    {
        RoutineComputerPolicy routine(vault);
        const auto results = RunBenchmark(routine, cases, report.repetitions,
            [&routine](const ComputerSubgoal& subgoal) { routine.SetSubgoal(subgoal); });
        report.results.insert(report.results.end(), results.begin(), results.end());
    }

    // The stand-in baseline.
    {
        const ComputerSubgoal* current = nullptr;
        SubstringBaselinePolicy baseline(current);
        const auto results = RunBenchmark(baseline, cases, report.repetitions,
            [&current](const ComputerSubgoal& subgoal) { current = &subgoal; });
        report.results.insert(report.results.end(), results.begin(), results.end());
    }

    std::cout << report.Format();

    const BenchmarkResult routine = report.Total("routine");
    const BenchmarkResult baseline = report.Total("substring_baseline");

    Check(routine.cases == baseline.cases && routine.cases > 0,
        "The two providers were not asked the same number of cases, so nothing here "
        "is a comparison.");

    // The claim, stated as an assertion. Not "the routine policy is better at
    // everything" -- it abstains more, which costs model calls -- but that when it
    // does act, it does not act wrongly.
    Check(routine.wrong == 0,
        "The deterministic policy acted wrongly " + std::to_string(routine.wrong) +
            " time(s). A policy that guesses when unsure is one that has to be switched "
            "off.");
    Check(baseline.wrong > 0,
        "The substring stand-in got everything right, which means the case set has "
        "stopped containing the near-misses it exists to contain.");

    // And the measurement the whole feature is for.
    Check(routine.reachedModel == 0,
        "A deterministic decision reached a model.");
    Check(baseline.reachedModel == baseline.cases,
        "The baseline did not reach a model on every case, so the comparison is not "
        "between a model path and a model-free one.");

    std::cout << "\nSYNTHETIC, on " << routine.cases << " decisions: the deterministic policy "
              << "reached a model " << routine.reachedModel << " time(s) and the "
              << "model-driven stand-in " << baseline.reachedModel << ".\n";
    std::cout << "Wrong actions: routine " << routine.wrong << ", stand-in "
              << baseline.wrong << ". Declined: routine "
              << (routine.declinedCorrectly + routine.declinedUnnecessarily)
              << ", stand-in "
              << (baseline.declinedCorrectly + baseline.declinedUnnecessarily) << ".\n";
}

// A learned artifact is compared on the same cases, or the comparison says why not.
void TestALearnedArtifactIsComparedOnTheSameCases()
{
    PayloadVault vault;
    LearnedComputerPolicy learned(vault);
    Check(!learned.IsAvailable(),
        "A learned policy with no artifact reported itself ready.");

    // With nothing loaded there is nothing to compare, and the benchmark says so rather
    // than reporting a row of zeros that reads like a result.
    const std::vector<BenchmarkCase> cases = StandardBenchmarkCases(vault);
    const auto results = RunBenchmark(learned, cases, 1,
        [&learned](const ComputerSubgoal& subgoal) { learned.SetSubgoal(subgoal); });

    BenchmarkResult total;
    for (const BenchmarkResult& result : results)
    {
        total.cases += result.cases;
        total.correct += result.correct;
        total.wrong += result.wrong;
    }
    Check(total.cases == cases.size() && total.correct == 0 && total.wrong == 0,
        "An unloaded learned policy produced decisions.");
    std::cout << "\nThe learned backend was asked all " << total.cases
              << " cases with no artifact loaded and acted on none of them.\n";
}

} // namespace

void RunComputerBenchmarkTests()
{
    TestTheProvidersAreComparedOnTheSameProblems();
    TestALearnedArtifactIsComparedOnTheSameCases();
    std::cout << "Synthetic provider comparison is reproducible on identical "
                 "problems. It is not a measurement of Main; --computer-live is.\n";
}
