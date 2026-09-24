#include "testSupport.h"

#include "Computer/learnedPolicy.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// The deployment half of the parity check.
//
//   ReviaTests.exe --computer-parity <cases.json> <expected.json>
//
// `Tools/Computer/parity.py` writes what Python computes for a set of cases. This reads
// the same cases, computes the same things in C++, and compares.
//
// Worth the trouble because a feature drift between training and deployment is the
// quietest failure in the pipeline. Nothing crashes. The model still loads, still ranks,
// still returns a candidate. It is simply answering a different question than the one it
// was evaluated on -- and the held-out numbers that justified shipping it were measured
// on the other one.

namespace
{

using namespace revia::computer;
using revia::tests::Check;

ObservedCandidate ReadCandidate(const nlohmann::json& entry)
{
    ObservedCandidate candidate;
    candidate.name = entry.value("name", std::string{});
    candidate.role = entry.value("role", std::string{});
    candidate.mayInvoke = entry.value("may_invoke", false);
    // The Python side carries one "may_edit"; the C++ observation distinguishes setting
    // text from typing it, and the feature uses either. Mapped onto maySetText so the
    // two describe the same candidate.
    candidate.maySetText = entry.value("may_edit", false);
    // The context a nameless control is identified by. Version 2 of the feature
    // contract reads both, so parity has to carry both or the two sides are comparing
    // different candidates while agreeing about the arithmetic.
    candidate.inferredLabel = entry.value("inferred_label", std::string{});
    candidate.container = entry.value("container", std::string{});
    candidate.nameless = candidate.name.empty();
    return candidate;
}

// A subgoal as the case file describes one.
//
// Built by hand rather than through ValidateSubgoal: parity is about the feature
// arithmetic, and going through validation would mean inventing a scope and a vault for
// every case to check something neither of them affects.
ComputerSubgoal ReadSubgoal(const nlohmann::json& entry)
{
    ComputerSubgoal subgoal;
    subgoal.intent = SubgoalIntentFromString(entry.value("intent", std::string{}));
    subgoal.target.name = entry.value("target_name", std::string{});
    subgoal.target.role = entry.value("target_role", std::string{});
    subgoal.target.container = entry.value("target_container", std::string{});
    return subgoal;
}

std::vector<double> Softmax(const std::vector<double>& scores)
{
    if (scores.empty()) return {};
    const double highest = *std::max_element(scores.begin(), scores.end());
    std::vector<double> exponentials;
    double total = 0.0;
    for (const double score : scores)
    {
        const double value = std::exp(score - highest);
        exponentials.push_back(value);
        total += value;
    }
    for (double& value : exponentials) value /= total;
    return exponentials;
}

} // namespace

void RunComputerParity(const std::string& casesPath, const std::string& expectedPath)
{
    std::ifstream casesFile(casesPath);
    Check(static_cast<bool>(casesFile),
        "The parity cases could not be read from " + casesPath +
            ". Run Tools/Computer/parity.py with --write-cases first.");
    const nlohmann::json cases = nlohmann::json::parse(casesFile, nullptr, false);
    Check(!cases.is_discarded() && cases.is_array(),
        "The parity cases are not a JSON array.");

    std::ifstream expectedFile(expectedPath);
    Check(static_cast<bool>(expectedFile),
        "Python's answers could not be read from " + expectedPath + ".");
    const nlohmann::json expected = nlohmann::json::parse(expectedFile, nullptr, false);
    Check(!expected.is_discarded() && expected.is_object(),
        "Python's answers are not a JSON object.");

    const auto weights = expected.at("weights").get<std::vector<double>>();
    const double abstainBelow = expected.at("abstain_below").get<double>();
    const double featureTolerance = expected.value("feature_tolerance", 1e-12);
    const double scoreTolerance = expected.value("score_tolerance", 1e-9);
    const auto expectedCases = expected.at("cases");

    Check(expectedCases.size() == cases.size(),
        "The two halves were given different numbers of cases.");

    // The feature contract itself, before any arithmetic. A reordering here would make
    // every comparison below pass while the weights landed on the wrong columns.
    const auto pythonNames =
        expected.at("feature_names").get<std::vector<std::string>>();
    {
        ComputerSubgoal probe;
        probe.intent = SubgoalIntent::InteractWithControl;
        ObservedCandidate candidate;
        Check(CandidateFeatures(probe, candidate, 0, 1).size() == pythonNames.size(),
            "The two halves compute different numbers of features: Python names " +
                std::to_string(pythonNames.size()) + ".");
    }

    std::size_t compared = 0;
    double largestFeatureGap = 0.0;
    double largestScoreGap = 0.0;

    for (std::size_t index = 0; index < cases.size(); ++index)
    {
        const nlohmann::json& testCase = cases[index];
        const nlohmann::json& answer = expectedCases[index];
        const std::string name = testCase.value("name", std::string{});
        Check(answer.value("name", std::string{}) == name,
            "The two halves disagree about which case is which at position " +
                std::to_string(index) + ".");

        const ComputerSubgoal subgoal = ReadSubgoal(testCase.at("subgoal"));
        std::vector<ObservedCandidate> candidates;
        for (const auto& entry : testCase.at("candidates"))
        {
            candidates.push_back(ReadCandidate(entry));
        }

        const auto expectedFeatures =
            answer.at("features").get<std::vector<std::vector<double>>>();
        Check(expectedFeatures.size() == candidates.size(),
            "Case \"" + name + "\": Python produced a different number of feature rows.");

        std::vector<double> scores;
        for (std::size_t row = 0; row < candidates.size(); ++row)
        {
            const std::vector<double> ours =
                CandidateFeatures(subgoal, candidates[row], row, candidates.size());
            const std::vector<double>& theirs = expectedFeatures[row];
            Check(ours.size() == theirs.size(),
                "Case \"" + name + "\": feature vectors are different lengths.");

            for (std::size_t column = 0; column < ours.size(); ++column)
            {
                const double gap = std::fabs(ours[column] - theirs[column]);
                largestFeatureGap = std::max(largestFeatureGap, gap);
                Check(gap <= featureTolerance,
                    "Case \"" + name + "\", candidate " + std::to_string(row) +
                        ", feature \"" + pythonNames[column] + "\": C++ says " +
                        std::to_string(ours[column]) + ", Python says " +
                        std::to_string(theirs[column]) + ".");
            }

            double score = 0.0;
            for (std::size_t column = 0; column < ours.size(); ++column)
            {
                score += weights[column] * ours[column];
            }
            scores.push_back(score);
        }

        const auto expectedScores = answer.at("scores").get<std::vector<double>>();
        Check(expectedScores.size() == scores.size(),
            "Case \"" + name + "\": different numbers of scores.");
        for (std::size_t row = 0; row < scores.size(); ++row)
        {
            const double gap = std::fabs(scores[row] - expectedScores[row]);
            largestScoreGap = std::max(largestScoreGap, gap);
            Check(gap <= scoreTolerance,
                "Case \"" + name + "\", candidate " + std::to_string(row) +
                    ": scores differ by " + std::to_string(gap) + ".");
        }

        // The two things the runtime acts on: which candidate, and whether to act at
        // all. Compared last because they are what the arithmetic above is for.
        const std::vector<double> probabilities = Softmax(scores);
        int chosen = -1;
        if (!probabilities.empty())
        {
            const auto best = static_cast<int>(std::distance(
                probabilities.begin(),
                std::max_element(probabilities.begin(), probabilities.end())));
            if (probabilities[static_cast<std::size_t>(best)] >= abstainBelow)
            {
                chosen = best;
            }
        }
        Check(chosen == answer.value("chosen", -1),
            "Case \"" + name + "\": C++ chose candidate " + std::to_string(chosen) +
                " and Python chose " + std::to_string(answer.value("chosen", -1)) +
                ". The two halves would take different actions on the same screen.");
        Check((chosen < 0) == answer.value("abstained", true),
            "Case \"" + name + "\": the two halves disagree about whether to act.");

        ++compared;
    }

    std::cout << "Parity: " << compared << " case(s) agree.\n";
    std::cout << std::scientific << std::setprecision(3);
    std::cout << "  largest feature difference: " << largestFeatureGap << "\n";
    std::cout << "  largest score difference:   " << largestScoreGap << "\n";
    std::cout << std::defaultfloat;
    std::cout << "Training and deployment compute the same features, the same scores, "
                 "and take the same decisions.\n";
}
