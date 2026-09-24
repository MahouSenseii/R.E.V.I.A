#pragma once

#include "Computer/computerController.h"
#include "Computer/computerSubgoal.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace revia::computer

{

// Comparing the providers on the same problems, with the comparison written down.
//
// The claim this feature makes is that routine decisions can leave the model. That claim
// is only meaningful against a baseline measured on the same tasks, in the same order,
// from the same observations -- otherwise "fewer model calls" is a statement about which
// tasks happened to come up.
//
// So a benchmark case is an observation and a subgoal, fixed in advance, and every
// provider is asked the same ones. What comes back is a count, not an impression.
//
// Deliberately headless. A comparison that needed a desktop could not be run repeatedly,
// could not be run in CI, and would be measuring the machine's state as much as the
// policies. The controlled GUI demonstration is where the real screen is exercised; this
// is where the decisions are counted.

// One problem, and what a correct answer to it looks like.
struct BenchmarkCase
{
    std::string name;
    // Which family this belongs to, so results can be read per kind of problem rather
    // than as one average that hides which kind got worse.
    std::string family;
    ComputerSubgoal subgoal;
    ComputerObservation observation;

    // What a correct decision does.
    //
    // Three kinds of right answer, not two. A policy may act on the named control; it
    // may decline, which is right when the screen does not identify one target; or it
    // may do something *else* that is nevertheless correct -- most often focusing the
    // window the target lives in before touching it.
    //
    // That third case was originally scored as a wrong action, and the benchmark said
    // the deterministic policy acted wrongly twenty times. It had not: it had proposed
    // bringing the right window forward, which is the correct bounded step. A benchmark
    // that scores correct behaviour as a failure is worse than no benchmark, because it
    // is the kind of failure somebody fixes by changing the policy.
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

// The standard case set.
//
// Built here rather than in a test so that the benchmark and the tests measure the same
// problems, and so a later provider cannot be evaluated on an easier set by accident.
// The vault is the caller's, and the cases reference payloads stored in it.
//
// Passed in rather than owned here because the policies under test hold a reference to
// one: a benchmark with its own vault would hand every policy a payload reference it
// cannot redeem, and every entry case would score as a refusal that looks like caution.
// `application` is the executable every case's subgoal names.
//
// A parameter rather than a constant because a learned artifact carries a qualified
// scope and the runtime refuses it outside that scope -- correctly. Running the
// comparison on cases in an application the artifact was never evaluated on measures the
// scope gate and reports it as the ranker's coverage, which is how a perfectly good
// artifact came back as 0 of 240 and looked like a failed model.
[[nodiscard]] std::vector<BenchmarkCase> StandardBenchmarkCases(
    PayloadVault& vault, const std::string& application = "benchmarkapp.exe");

// Run every case against one policy.
//
// The policy is asked directly rather than through the controller, because what is being
// compared is the policies -- routing them through a coordinator that may fall back would
// measure the fallback rather than the provider.
[[nodiscard]] std::vector<BenchmarkResult> RunBenchmark(
    IComputerPolicy& policy,
    const std::vector<BenchmarkCase>& cases,
    std::uint32_t repetitions,
    // Called before each case so a policy that needs its subgoal installed gets it. The
    // benchmark does not know which policies those are and does not need to.
    const std::function<void(const ComputerSubgoal&)>& installSubgoal);

} // namespace revia::computer
