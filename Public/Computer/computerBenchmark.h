#pragma once

#include "Computer/computerController.h"
#include "Computer/computerSubgoal.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace revia::computer

{

// Headless policy comparisons use fixed subgoals and observations;
// each provider receives the same cases in the same order.

// One problem, and what a correct answer to it looks like.
struct BenchmarkCase
{
    std::string name;
    // Which family this belongs to, so results can be read per kind of problem rather
    // than as one average that hides which kind got worse.
    std::string family;
    ComputerSubgoal subgoal;
    ComputerObservation observation;

    // A correct decision may act, decline an ambiguous target, or take a valid
    // preparatory action such as focusing the target's window.
    revia::actions::ActionType expectedAction = revia::actions::ActionType::Unknown;
    // The control for an interaction or an entry; the application for a focus.
    std::string expectedTarget;
    bool expectedToDecline = false;
};

// What one provider did on one family.
struct BenchmarkResult
{
    std::string provider;
    std::string family;
    std::uint32_t cases = 0;
    // Proposed an action and named the right control.
    std::uint32_t correct = 0;
    // Proposed an action and named the wrong one. The number that matters most: an
    // abstention costs a model call, this costs an action nobody authorized.
    std::uint32_t wrong = 0;
    // Declined, correctly.
    std::uint32_t declinedCorrectly = 0;
    // Declined when it should have acted. A cost, not a danger.
    std::uint32_t declinedUnnecessarily = 0;
    // Asked a person, which is neither acting nor declining.
    std::uint32_t escalatedToUser = 0;
    std::uint32_t reachedModel = 0;
    std::uint64_t totalMicroseconds = 0;

    [[nodiscard]] double Accuracy() const
    {
        const std::uint32_t answered = correct + wrong;
        return answered == 0 ? 0.0
            : static_cast<double>(correct) / static_cast<double>(answered);
    }
    [[nodiscard]] double Coverage() const
    {
        return cases == 0 ? 0.0
            : static_cast<double>(correct + wrong) / static_cast<double>(cases);
    }
    [[nodiscard]] double MicrosecondsPerCase() const
    {
        return cases == 0 ? 0.0
            : static_cast<double>(totalMicroseconds) / static_cast<double>(cases);
    }
};

// The whole comparison, per provider and per family.
struct BenchmarkReport
{
    std::uint32_t repetitions = 1;
    std::vector<BenchmarkResult> results;

    // Totals across families, for the one headline number the design asks for.
    [[nodiscard]] BenchmarkResult Total(const std::string& provider) const;
    [[nodiscard]] std::vector<std::string> Providers() const;
    [[nodiscard]] std::vector<std::string> Families() const;
    [[nodiscard]] std::string Format() const;
};

// Cases reference payloads in the caller's vault, shared with the tested policies.
// Choose an application within the learned artifact's qualified scope.
[[nodiscard]] std::vector<BenchmarkCase> StandardBenchmarkCases(PayloadVault& vault, const std::string& application = "benchmarkapp.exe");

// Calls the policy directly so fallback routing cannot hide its results.
[[nodiscard]] std::vector<BenchmarkResult> RunBenchmark(
    IComputerPolicy& policy,
    const std::vector<BenchmarkCase>& cases,
    std::uint32_t repetitions,
    // Called before each case so a policy that needs its subgoal installed gets it. The
    // benchmark does not know which policies those are and does not need to.
    const std::function<void(const ComputerSubgoal&)>& installSubgoal);

} // namespace revia::computer
