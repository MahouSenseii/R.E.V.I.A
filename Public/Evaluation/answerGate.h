#pragma once

#include "Evaluation/answerJudge.h"
#include "Evaluation/goldenSet.h"

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace revia::evaluation
{
struct GoldenTurnResult
{
    std::string input;
    std::string reply;
    std::string rawReply;
    bool repaired = false;
    std::vector<std::string> failures;
};

struct GoldenCaseResult
{
    std::string id;
    std::string category;
    // False when the model could not answer every turn. Such a case is unjudged.
    bool available = false;
    std::string unavailableReason;
    std::vector<GoldenTurnResult> turns;
    JudgeVerdict judge;

    [[nodiscard]] bool DeterministicPassed() const;
    // Deterministic checks and, when the judge answered, the judge's verdict.
    [[nodiscard]] bool Passed() const;
};

// Scores the replies the evaluator collected against the golden checks. The report must
// come from ConversationEvaluator::Run over ToEvaluationCase of the same cases.
[[nodiscard]] std::vector<GoldenCaseResult> ScoreGoldenReplies(const std::vector<GoldenCase>& cases, const EvaluationReport& report);
[[nodiscard]] JudgeInput BuildJudgeInput(const GoldenCase& item, const GoldenCaseResult& result, const std::optional<std::string>& baseline);

struct CategorySummary
{
    std::size_t cases = 0;
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t unavailable = 0;
    std::size_t judged = 0;
    double judgeScoreTotal = 0.0;

    [[nodiscard]] double PassRate() const;
    [[nodiscard]] double MeanJudgeScore() const;
};

struct GateSummary
{
    std::string timestamp;
    std::string model;
    std::string judge;
    std::string corpusDigest;
    std::map<std::string, CategorySummary> categories;
    CategorySummary overall;
    std::size_t preferredCandidate = 0;
    std::size_t preferredBaseline = 0;
    std::size_t ties = 0;
};

[[nodiscard]] GateSummary SummarizeGoldenResults(const std::vector<GoldenCaseResult>& results);
[[nodiscard]] nlohmann::json GateSummaryJson(const GateSummary& summary);
[[nodiscard]] nlohmann::json GoldenCaseResultJson(const GoldenCaseResult& result);

// The accepted run that later runs are compared against. It is a checked-in file, so
// replacing it is a reviewed change rather than a side effect of a lucky night.
struct BaselineCase
{
    bool deterministicPassed = false;
    std::optional<bool> judgePassed;
    int judgeScore = 0;
    std::string finalReply;
};

struct GateBaseline
{
    std::string createdAt;
    std::string model;
    std::string judge;
    std::string corpusDigest;
    std::map<std::string, BaselineCase> cases;
};

[[nodiscard]] nlohmann::json GateBaselineJson(const std::vector<GoldenCaseResult>& results, const GateSummary& summary);
[[nodiscard]] bool ParseGateBaseline(const std::string& bytes, GateBaseline& outBaseline, std::string& outError);

struct GateOptions
{
    // Percentage points the overall pass rate may fall below the baseline.
    double maximumDropPoints = 3.0;
    // More unjudged cases than this makes the run incomplete rather than comparable.
    double maximumUnavailablePercent = 5.0;
    // With the judge enabled, fewer judge verdicts than this makes the run incomplete.
    double minimumJudgeCoveragePercent = 90.0;
    bool judgeEnabled = true;
};

enum class GateOutcome
{
    Passed,
    Regressed,
    Incomplete,
    NoBaseline
};

[[nodiscard]] std::string ToString(GateOutcome outcome);

struct GateDecision
{
    GateOutcome outcome = GateOutcome::Incomplete;
    std::size_t comparedCases = 0;
    double currentPassRate = 0.0;
    double baselinePassRate = 0.0;
    double dropPoints = 0.0;
    std::vector<std::string> notes;

    [[nodiscard]] int ExitCode() const;
};

// Compares on the cases both runs answered, so adding or retiring a case does not move
// the comparison by itself. A case either side could not judge falls back to its
// deterministic result on both sides.
[[nodiscard]] GateDecision DecideGate(
    const std::vector<GoldenCaseResult>& results, const GateBaseline* baseline, const GateOptions& options);
[[nodiscard]] nlohmann::json GateDecisionJson(const GateDecision& decision);
}
