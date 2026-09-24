#include "testSupport.h"

#include "Computer/computerBenchmark.h"
#include "Computer/learnedPolicy.h"
#include "Computer/routinePolicy.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>

// Does the learned ranker earn its place?
//
//   ReviaTests.exe --learned-compare <artifact.json>
//
// The question section 9 of the design asks and this answers with a number: a learned
// policy has to beat the deterministic one before there is any reason to prefer it, and
// "beat" means more coverage without more wrong actions. A ranker that matches the
// baseline is a ranker that added a training pipeline, an artifact format, a
// qualification gate and a parity check in exchange for nothing.
//
// Both policies are asked the same standard cases, which is the same set the synthetic
// comparison uses. The artifact is loaded through the shipping loader, so an artifact
// that fails its own gates fails here too and says why.

namespace
{

using namespace revia::computer;
using revia::tests::Check;

void PrintRow(const std::string& label, const BenchmarkResult& total)
{
    std::cout << "  " << std::left << std::setw(22) << label
              << std::right
              << std::setw(7) << total.cases
              << std::setw(9) << total.correct
              << std::setw(7) << total.wrong
              << std::setw(11) << (total.declinedCorrectly + total.declinedUnnecessarily)
              << std::setw(11) << std::fixed << std::setprecision(2) << total.Coverage()
              << std::setw(11) << total.Accuracy()
              << std::setw(10) << std::setprecision(1) << total.MicrosecondsPerCase()
              << "\n";
}

} // namespace

void RunLearnedCompare(const std::string& artifactPath)
{
    std::cout << "\n===== learned ranker versus the deterministic policy =====\n";

    PayloadVault vault;
    constexpr std::uint32_t Repetitions = 20;

    // The artifact is loaded first, because it decides which cases the comparison can
    // honestly be run on.
    //
    // A qualified artifact names the application it was evaluated in, and the runtime
    // refuses it everywhere else. Running the standard cases under their own default
    // executable therefore measured the scope gate and printed the answer as the
    // ranker's coverage -- 0 of 240, which reads as a broken model and was nothing of
    // the kind. The cases are built in the artifact's own application instead, so both
    // providers are asked questions both are allowed to answer.
    LearnedComputerPolicy learned(vault);
    const ArtifactLoad load = learned.Load(artifactPath);
    const std::string application = load.accepted &&
        !load.artifact.qualifiedApplications.empty()
            ? load.artifact.qualifiedApplications.front()
            : std::string("benchmarkapp.exe");

    const std::vector<BenchmarkCase> cases = StandardBenchmarkCases(vault, application);

    BenchmarkReport report;
    report.repetitions = Repetitions;

    {
        RoutineComputerPolicy routine(vault);
        const auto results = RunBenchmark(routine, cases, Repetitions,
            [&routine](const ComputerSubgoal& subgoal) { routine.SetSubgoal(subgoal); });
        report.results.insert(report.results.end(), results.begin(), results.end());
    }

    std::cout << "\nArtifact: " << artifactPath << "\n";
    if (!load.accepted)
    {
        // A refusal here is a result, not a missing run. The gates exist so that an
        // artifact which cannot support its own claims never decides anything, and an
        // artifact refused at load is one the routine policy keeps working past.
        std::cout << "  REFUSED at load: " << ToString(load.rejection) << "\n";
        std::cout << "  " << load.detail << "\n";
        std::cout << "\nThe deterministic policy is unaffected and keeps deciding.\n";
        const BenchmarkResult routineTotal = report.Total("routine");
        std::cout << "\n  " << std::left << std::setw(22) << "provider"
                  << std::right << std::setw(7) << "cases" << std::setw(9) << "correct"
                  << std::setw(7) << "wrong" << std::setw(11) << "declined"
                  << std::setw(11) << "coverage" << std::setw(11) << "accuracy"
                  << std::setw(10) << "us/case" << "\n";
        PrintRow("routine", routineTotal);
        return;
    }

    const LearnedArtifact& artifact = learned.Artifact();
    std::cout << "  cases built in: " << application
              << "   (the artifact's own qualified application)\n";
    std::cout << "  loaded.  held-out examples: " << artifact.heldOutExamples
              << ", coverage " << artifact.heldOutCoverage
              << ", accuracy " << artifact.heldOutAccuracy
              << ", confident mistakes " << artifact.heldOutWrongAndConfident << "\n";
    std::cout << "  qualified for: ";
    for (const std::string& application : artifact.qualifiedApplications)
    {
        std::cout << application << ' ';
    }
    std::cout << "/ ";
    for (const std::string& intent : artifact.qualifiedIntents) std::cout << intent << ' ';
    std::cout << "\n";

    // What the weights actually learned. Printed because it is the part a summary
    // statistic hides, and because a weight on position is a weight on this fixture's
    // layout rather than on anything about the task.
    std::cout << "\n  weights:\n";
    for (std::size_t index = 0; index < artifact.featureNames.size(); ++index)
    {
        if (std::fabs(artifact.weights[index]) < 1e-6) continue;
        std::cout << "    " << std::left << std::setw(26) << artifact.featureNames[index]
                  << std::showpos << std::fixed << std::setprecision(4)
                  << artifact.weights[index] << std::noshowpos << "\n";
    }

    {
        const auto results = RunBenchmark(learned, cases, Repetitions,
            [&learned](const ComputerSubgoal& subgoal) { learned.SetSubgoal(subgoal); });
        report.results.insert(report.results.end(), results.begin(), results.end());
    }

    const BenchmarkResult routineTotal = report.Total("routine");
    const BenchmarkResult learnedTotal = report.Total("learned");

    std::cout << "\n  " << std::left << std::setw(22) << "provider"
              << std::right << std::setw(7) << "cases" << std::setw(9) << "correct"
              << std::setw(7) << "wrong" << std::setw(11) << "declined"
              << std::setw(11) << "coverage" << std::setw(11) << "accuracy"
              << std::setw(10) << "us/case" << "\n";
    PrintRow("routine", routineTotal);
    PrintRow("learned", learnedTotal);

    std::cout << "\n  verdict: ";
    if (learnedTotal.wrong > routineTotal.wrong)
    {
        std::cout << "the learned ranker acts wrongly more often ("
                  << learnedTotal.wrong << " vs " << routineTotal.wrong
                  << "). Retain the deterministic policy.\n";
    }
    else if (learnedTotal.correct > routineTotal.correct &&
             learnedTotal.wrong <= routineTotal.wrong)
    {
        std::cout << "the learned ranker covers more without acting wrongly more often. "
                     "It is worth evaluating further.\n";
    }
    else if (learnedTotal.correct == routineTotal.correct &&
             learnedTotal.wrong == routineTotal.wrong)
    {
        std::cout << "the learned ranker matches the deterministic policy exactly. "
                     "Matching is not beating: it would add a training pipeline, an "
                     "artifact format and a qualification gate for no change in "
                     "behaviour. Retain the deterministic policy.\n";
    }
    else
    {
        std::cout << "the learned ranker covers less than the deterministic policy ("
                  << learnedTotal.correct << " correct vs " << routineTotal.correct
                  << "). Retain the deterministic policy.\n";
    }

    // Coverage has two quite different causes and the verdict above cannot tell them
    // apart on its own. A ranker that abstained because the ranking was unclear is
    // behaving as designed; one that abstained because the intent is outside the scope
    // it was evaluated on never had an opinion to offer.
    std::size_t outsideIntent = 0;
    for (const BenchmarkCase& benchmarkCase : cases)
    {
        const std::string intent = ToString(benchmarkCase.subgoal.intent);
        if (std::find(artifact.qualifiedIntents.begin(), artifact.qualifiedIntents.end(),
                intent) == artifact.qualifiedIntents.end())
        {
            ++outsideIntent;
        }
    }
    if (outsideIntent > 0)
    {
        std::cout << "\n  " << outsideIntent << " of " << cases.size()
                  << " case(s) ask for an intent this artifact was never evaluated on. "
                     "It declines\n  those by construction, and that is a statement "
                     "about its scope rather than about its ranking.\n";
    }

    std::cout << "\n  This comparison is on the standard synthetic cases. It says what "
                 "the artifact\n  does on those problems and nothing about any other "
                 "software.\n";
}
