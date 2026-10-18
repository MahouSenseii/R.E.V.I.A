#include "Evaluation/answerGate.h"

#include "Agents/conversationQualityMonitor.h"

#include <stdexcept>

namespace revia::evaluation
{
namespace
{
using Json = nlohmann::json;

Json CategoryJson(const CategorySummary& category)
{
    return {{"cases", category.cases}, {"passed", category.passed}, {"failed", category.failed}, {"unavailable", category.unavailable},
        {"judged", category.judged}, {"passRate", category.PassRate()}, {"meanJudgeScore", category.MeanJudgeScore()}};
}

void Count(CategorySummary& category, const GoldenCaseResult& result)
{
    ++category.cases;
    if (!result.available)
    {
        ++category.unavailable;
        return;
    }
    result.Passed() ? ++category.passed : ++category.failed;
    if (result.judge.available)
    {
        ++category.judged;
        category.judgeScoreTotal += result.judge.score;
    }
}

double Percent(const std::size_t part, const std::size_t whole)
{
    return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}
}

bool GoldenCaseResult::DeterministicPassed() const
{
    if (!available || turns.empty())
        return false;
    for (const auto& turn : turns)
        if (!turn.failures.empty())
            return false;
    return true;
}

bool GoldenCaseResult::Passed() const
{
    return DeterministicPassed() && (!judge.available || judge.pass);
}

std::vector<GoldenCaseResult> ScoreGoldenReplies(const std::vector<GoldenCase>& cases, const EvaluationReport& report)
{
    if (cases.size() != report.cases.size())
        throw std::runtime_error("The evaluation report does not cover the golden set.");
    std::vector<GoldenCaseResult> results;
    results.reserve(cases.size());
    for (std::size_t index = 0; index < cases.size(); ++index)
    {
        const auto& item = cases[index];
        const auto& outcome = report.cases[index];
        if (outcome.id != item.id || outcome.turns.size() > item.turns.size())
            throw std::runtime_error("The evaluation report case " + outcome.id + " does not match golden case " + item.id + ".");
        GoldenCaseResult result;
        result.id = item.id;
        result.category = item.category;
        result.available = !outcome.unavailable && outcome.turns.size() == item.turns.size();
        std::vector<std::string> openings;
        for (std::size_t turnIndex = 0; turnIndex < outcome.turns.size(); ++turnIndex)
        {
            const auto& turn = outcome.turns[turnIndex];
            GoldenTurnResult scored;
            scored.input = turn.input;
            scored.reply = turn.reply;
            scored.rawReply = turn.rawReply;
            scored.repaired = turn.repaired;
            scored.failures = turn.failures;
            if (!turn.modelSucceeded)
            {
                result.available = false;
                result.unavailableReason = turn.failures.empty() ? "The model did not answer." : turn.failures.front();
            }
            else
            {
                for (auto& failure : CheckGoldenTurn(item.turns[turnIndex], turn.reply, openings))
                    scored.failures.push_back(std::move(failure));
                const auto opening = agents::ConversationQualityMonitor::OpeningOf(turn.reply);
                if (!opening.empty())
                    openings.push_back(opening);
            }
            result.turns.push_back(std::move(scored));
        }
        if (!result.available && result.unavailableReason.empty())
            result.unavailableReason = "The run stopped before this case finished.";
        results.push_back(std::move(result));
    }
    return results;
}

JudgeInput BuildJudgeInput(const GoldenCase& item, const GoldenCaseResult& result, const std::optional<std::string>& baseline)
{
    if (result.turns.empty() || result.turns.size() != item.turns.size())
        throw std::runtime_error("Only a complete case can be judged.");
    JudgeInput input;
    input.caseId = item.id;
    input.category = item.category;
    input.rubric = item.rubric;
    for (std::size_t index = 0; index + 1 < result.turns.size(); ++index)
    {
        input.priorTurns.push_back({"user", result.turns[index].input});
        input.priorTurns.push_back({"assistant", result.turns[index].reply});
    }
    input.finalInput = result.turns.back().input;
    input.candidate = result.turns.back().reply;
    input.baseline = baseline;
    return input;
}

double CategorySummary::PassRate() const
{
    return Percent(passed, passed + failed);
}

double CategorySummary::MeanJudgeScore() const
{
    return judged == 0 ? 0.0 : judgeScoreTotal / static_cast<double>(judged);
}

GateSummary SummarizeGoldenResults(const std::vector<GoldenCaseResult>& results)
{
    GateSummary summary;
    for (const auto& result : results)
    {
        Count(summary.overall, result);
        Count(summary.categories[result.category], result);
        if (!result.available || !result.judge.available)
            continue;
        if (result.judge.preference == JudgePreference::Candidate)
            ++summary.preferredCandidate;
        else if (result.judge.preference == JudgePreference::Baseline)
            ++summary.preferredBaseline;
        else if (result.judge.preference == JudgePreference::Tie)
            ++summary.ties;
    }
    return summary;
}

Json GateSummaryJson(const GateSummary& summary)
{
    Json categories = Json::object();
    for (const auto& [name, category] : summary.categories)
        categories[name] = CategoryJson(category);
    return {{"schemaVersion", 1}, {"timestamp", summary.timestamp}, {"model", summary.model}, {"judge", summary.judge},
        {"corpusDigest", summary.corpusDigest}, {"overall", CategoryJson(summary.overall)}, {"categories", categories},
        {"pairwise", {{"candidate", summary.preferredCandidate}, {"baseline", summary.preferredBaseline}, {"tie", summary.ties}}}};
}

Json GoldenCaseResultJson(const GoldenCaseResult& result)
{
    Json turns = Json::array();
    for (const auto& turn : result.turns)
        turns.push_back({{"input", turn.input}, {"reply", turn.reply}, {"rawReply", turn.rawReply}, {"repaired", turn.repaired},
            {"failures", turn.failures}});
    const auto& judge = result.judge;
    return {{"record", "case"}, {"id", result.id}, {"category", result.category}, {"available", result.available},
        {"unavailableReason", result.unavailableReason}, {"deterministicPassed", result.DeterministicPassed()},
        {"passed", result.available && result.Passed()}, {"turns", turns},
        {"judge", {{"available", judge.available}, {"score", judge.score}, {"pass", judge.pass}, {"reason", judge.reason},
                      {"pairwise", judge.pairwise}, {"preference", ToString(judge.preference)}, {"judgeId", judge.judgeId},
                      {"error", judge.error}}}};
}

Json GateBaselineJson(const std::vector<GoldenCaseResult>& results, const GateSummary& summary)
{
    Json cases = Json::object();
    for (const auto& result : results)
    {
        if (!result.available)
            continue;
        cases[result.id] = {{"deterministicPassed", result.DeterministicPassed()},
            {"judgePassed", result.judge.available ? Json(result.judge.pass) : Json(nullptr)},
            {"judgeScore", result.judge.available ? result.judge.score : 0}, {"finalReply", result.turns.back().reply}};
    }
    return {{"schemaVersion", 1}, {"createdAt", summary.timestamp}, {"model", summary.model}, {"judge", summary.judge},
        {"corpusDigest", summary.corpusDigest}, {"summary", GateSummaryJson(summary)}, {"cases", cases}};
}

bool ParseGateBaseline(const std::string& bytes, GateBaseline& outBaseline, std::string& outError)
{
    try
    {
        const auto parsed = Json::parse(bytes);
        if (!parsed.is_object() || parsed.value("schemaVersion", 0) != 1 || !parsed.contains("cases") || !parsed.at("cases").is_object())
            throw std::runtime_error("Unsupported baseline schema.");
        GateBaseline baseline;
        baseline.createdAt = parsed.value("createdAt", std::string());
        baseline.model = parsed.value("model", std::string());
        baseline.judge = parsed.value("judge", std::string());
        baseline.corpusDigest = parsed.value("corpusDigest", std::string());
        for (const auto& [id, entry] : parsed.at("cases").items())
        {
            if (!entry.is_object() || !entry.contains("deterministicPassed") || !entry.at("deterministicPassed").is_boolean() ||
                !entry.contains("finalReply") || !entry.at("finalReply").is_string())
                throw std::runtime_error("Baseline case " + id + " is incomplete.");
            BaselineCase item;
            item.deterministicPassed = entry.at("deterministicPassed").get<bool>();
            if (entry.contains("judgePassed") && entry.at("judgePassed").is_boolean())
                item.judgePassed = entry.at("judgePassed").get<bool>();
            else if (entry.contains("judgePassed") && !entry.at("judgePassed").is_null())
                throw std::runtime_error("Baseline case " + id + " has an invalid judge verdict.");
            item.judgeScore = entry.value("judgeScore", 0);
            item.finalReply = entry.at("finalReply").get<std::string>();
            baseline.cases.emplace(id, std::move(item));
        }
        if (baseline.cases.empty())
            throw std::runtime_error("The baseline holds no cases.");
        outBaseline = std::move(baseline);
        outError.clear();
        return true;
    }
    catch (const std::exception& error)
    {
        outError = error.what();
        return false;
    }
}

std::string ToString(const GateOutcome outcome)
{
    switch (outcome)
    {
    case GateOutcome::Passed:
        return "passed";
    case GateOutcome::Regressed:
        return "regressed";
    case GateOutcome::Incomplete:
        return "incomplete";
    case GateOutcome::NoBaseline:
        return "no_baseline";
    }
    return "incomplete";
}

int GateDecision::ExitCode() const
{
    switch (outcome)
    {
    case GateOutcome::Passed:
    case GateOutcome::NoBaseline:
        return 0;
    case GateOutcome::Regressed:
        return 1;
    case GateOutcome::Incomplete:
        break;
    }
    return 2;
}

GateDecision DecideGate(const std::vector<GoldenCaseResult>& results, const GateBaseline* baseline, const GateOptions& options)
{
    GateDecision decision;
    std::size_t available = 0;
    std::size_t passed = 0;
    std::size_t judged = 0;
    for (const auto& result : results)
    {
        if (!result.available)
            continue;
        ++available;
        passed += result.Passed() ? 1 : 0;
        judged += result.judge.available ? 1 : 0;
    }
    decision.currentPassRate = Percent(passed, available);
    if (results.empty() || available == 0)
    {
        decision.notes.emplace_back("No case was answered; the model was unreachable or the run was stopped.");
        return decision;
    }
    const double unavailablePercent = Percent(results.size() - available, results.size());
    if (unavailablePercent > options.maximumUnavailablePercent)
    {
        decision.notes.push_back(std::to_string(results.size() - available) + " of " + std::to_string(results.size()) +
                                 " cases were unjudged, above the allowed share; the run cannot be compared.");
        return decision;
    }
    if (options.judgeEnabled && Percent(judged, available) < options.minimumJudgeCoveragePercent)
    {
        // Without verdicts, the pass rate would quietly become a phrase-check rate and look better than the baseline's.
        decision.notes.push_back("The judge answered " + std::to_string(judged) + " of " + std::to_string(available) +
                                 " cases, below the required coverage; the run cannot be compared.");
        return decision;
    }
    if (baseline == nullptr)
    {
        decision.outcome = GateOutcome::NoBaseline;
        decision.notes.emplace_back("No baseline is stored, so nothing was compared. Write one deliberately with --write-baseline.");
        return decision;
    }
    std::size_t currentPassed = 0;
    std::size_t baselinePassed = 0;
    for (const auto& result : results)
    {
        const auto found = baseline->cases.find(result.id);
        if (!result.available || found == baseline->cases.end())
            continue;
        const auto& base = found->second;
        const bool useJudge = options.judgeEnabled && result.judge.available && base.judgePassed.has_value();
        currentPassed += result.DeterministicPassed() && (!useJudge || result.judge.pass) ? 1 : 0;
        baselinePassed += base.deterministicPassed && (!useJudge || *base.judgePassed) ? 1 : 0;
        ++decision.comparedCases;
    }
    if (decision.comparedCases == 0)
    {
        decision.notes.emplace_back("The baseline shares no answered case with this run.");
        return decision;
    }
    decision.currentPassRate = Percent(currentPassed, decision.comparedCases);
    decision.baselinePassRate = Percent(baselinePassed, decision.comparedCases);
    decision.dropPoints = decision.baselinePassRate - decision.currentPassRate;
    decision.outcome = decision.dropPoints > options.maximumDropPoints + 1e-9 ? GateOutcome::Regressed : GateOutcome::Passed;
    if (decision.comparedCases < available)
        decision.notes.push_back(std::to_string(available - decision.comparedCases) + " answered cases are not in the baseline and were not compared.");
    return decision;
}

Json GateDecisionJson(const GateDecision& decision)
{
    return {{"outcome", ToString(decision.outcome)}, {"exitCode", decision.ExitCode()}, {"comparedCases", decision.comparedCases},
        {"currentPassRate", decision.currentPassRate}, {"baselinePassRate", decision.baselinePassRate},
        {"dropPoints", decision.dropPoints}, {"notes", decision.notes}};
}
}
