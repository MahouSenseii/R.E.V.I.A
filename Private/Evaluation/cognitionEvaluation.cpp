#include "Evaluation/cognitionEvaluation.h"
#include "Audit/contentDigest.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

namespace revia::evaluation
{
namespace
{
using Json = nlohmann::json;

bool IsDigest(const std::string& value)
{
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

Json OptionalVerdict(const std::optional<bool>& value)
{
    return value ? Json(*value) : Json(nullptr);
}

Json ParseOracleJson(const std::string& bytes, std::size_t byteLimit = 262144)
{
    if (bytes.size() > byteLimit)
        throw std::runtime_error("Oracle JSON exceeds its limit.");
    std::vector<std::set<std::string>> keys;
    return Json::parse(bytes,
        [&](int depth, Json::parse_event_t event, Json& parsed)
        {
            if (depth > 32)
                throw std::runtime_error("Oracle JSON exceeds its depth limit.");
            if (event == Json::parse_event_t::object_start)
                keys.emplace_back();
            else if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
                throw std::runtime_error("Duplicate oracle JSON key.");
            else if (event == Json::parse_event_t::object_end)
                keys.pop_back();
            return true;
        });
}
}

std::string CognitionSourceBytes(const EvaluationCase& item)
{
    // A one-turn source uses its exact input; multiple turns retain role order.
    if (item.turns.size() == 1)
        return item.turns.front().input;
    Json inputs = Json::array();
    for (const auto& turn : item.turns)
        inputs.push_back(turn.input);
    return inputs.dump();
}

std::string CognitionSourceDigest(const EvaluationCase& item)
{
    return audit::ContentDigest(CognitionSourceBytes(item));
}

std::string CognitionOutputBytes(const CaseOutcome& output)
{
    Json turns = Json::array();
    for (const auto& turn : output.turns)
        turns.push_back({{"input", turn.input}, {"reply", turn.reply}, {"rawReply", turn.rawReply}, {"modelSucceeded", turn.modelSucceeded},
            {"failures", turn.failures}});
    return Json{{"caseId", output.id}, {"unavailable", output.unavailable}, {"turns", turns}}.dump();
}

std::string CognitionCriterionDigest(const CognitionCase& item)
{
    return audit::ContentDigest(
        Json{{"id", item.id}, {"family", item.family}, {"oracleId", item.oracleId}, {"sourceDigest", item.sourceDigest},
            {"expectedJson", item.expectedJson}, {"entityIds", item.entityIds}, {"requiredClaims", item.requiredClaims},
            {"forbiddenClaims", item.forbiddenClaims}, {"uncertaintyDisposition", item.uncertaintyDisposition}}
            .dump());
}

OracleRegistry::OracleRegistry()
{
    Register("json-exact-v1",
        [](const CognitionCase& item, const CaseOutcome& output) -> std::optional<bool>
        {
            if (output.turns.empty())
                return false;
            try
            {
                return ParseOracleJson(output.turns.back().reply).dump() == ParseOracleJson(item.expectedJson).dump();
            }
            catch (const std::exception&)
            {
                return false;
            }
        });
    Register("human-rubric-v1", [](const CognitionCase&, const CaseOutcome&) -> std::optional<bool> { return std::nullopt; });
}

bool OracleRegistry::Register(std::string id, Checker checker)
{
    if (id.empty() || !checker)
        return false;
    return checkers.emplace(std::move(id), std::move(checker)).second;
}

std::optional<bool> OracleRegistry::Judge(const CognitionCase& item, const CaseOutcome& output) const
{
    const auto found = checkers.find(item.oracleId);
    return found == checkers.end() ? std::nullopt : found->second(item, output);
}

bool OracleRegistry::Contains(const std::string& id) const
{
    return checkers.contains(id);
}

CognitionVerdict EvaluateCognitionCase(
    const CognitionCase& item, const CaseOutcome& output, const std::uint64_t seed, const OracleRegistry& oracles)
{
    CognitionVerdict verdict;
    verdict.caseId = item.id;
    verdict.seed = seed;
    verdict.sourceDigest = item.sourceDigest;
    verdict.criterionDigest = CognitionCriterionDigest(item);
    verdict.outputDigest = audit::ContentDigest(CognitionOutputBytes(output));
    verdict.available = !output.unavailable && !output.turns.empty();
    for (const auto& turn : output.turns)
        verdict.available = verdict.available && turn.modelSucceeded;
    verdict.bindingVerified = item.id == output.id && item.conversation.id == item.id && IsDigest(item.sourceDigest) &&
                              item.sourceDigest == CognitionSourceDigest(item.conversation) &&
                              output.turns.size() == item.conversation.turns.size();
    for (std::size_t index = 0; verdict.bindingVerified && index < output.turns.size(); ++index)
        verdict.bindingVerified = output.turns[index].input == item.conversation.turns[index].input;
    if (!verdict.bindingVerified)
    {
        verdict.available = false;
        verdict.diagnostic = "Source, case identity or turn binding mismatch.";
        return verdict;
    }
    if (!verdict.available)
    {
        verdict.diagnostic = "Cancelled or unavailable model output.";
        return verdict;
    }
    verdict.mechanicalPassed = std::all_of(output.turns.begin(), output.turns.end(), [](const TurnOutcome& turn) { return turn.Passed(); });
    verdict.semanticPassed = oracles.Judge(item, output);
    verdict.oraclePassed = verdict.semanticPassed;
    if (verdict.semanticPassed)
        verdict.reviewerId = "deterministic:" + item.oracleId;
    else
        verdict.diagnostic = oracles.Contains(item.oracleId) ? "Independent semantic review pending." : "Unknown oracle.";
    return verdict;
}

CognitionVerdict EvaluateBoundCognitionCase(const CognitionCase& item, const CaseOutcome& output, const CampaignManifest& campaign,
    const core::EvidenceBundle& evidence, const core::TaskContract& task, const runtime::RuntimeStamp& current,
    const memory::MemoryScope& scope, const core::CurrentStampGuard& guard, const OracleRegistry& oracles, std::stop_token cancellation)
{
    auto verdict = EvaluateCognitionCase(item, output, campaign.seed, oracles);
    std::string error;
    const auto admission = core::ValidateEvidenceAdmission(evidence, task, current, scope, guard, cancellation);
    const auto& references = evidence.references();
    const bool sourceBound = std::any_of(references.begin(), references.end(),
        [&](const auto& reference) { return reference.sourceId == "cognition-source" && reference.digest == verdict.sourceDigest; });
    const bool outputBound = std::any_of(references.begin(), references.end(),
        [&](const auto& reference) { return reference.sourceId == "cognition-output" && reference.digest == verdict.outputDigest; });
    if (!ValidateCampaignManifest(campaign, error) || !campaign.providerAvailable || !admission || !sourceBound || !outputBound ||
        task.stamp.sessionId != campaign.campaignId || task.stamp.taskId != item.id || task.sourceId != item.id ||
        task.stamp.attemptId != std::to_string(campaign.seed))
    {
        verdict.bindingVerified = false;
        verdict.semanticPassed.reset();
        verdict.oraclePassed.reset();
        verdict.personalityPassed.reset();
        verdict.reviewerId.clear();
        verdict.diagnostic = "Campaign, task or retained source/output evidence admission failed.";
        return verdict;
    }
    verdict.campaignDigest = audit::ContentDigest(SerializeCampaignManifest(campaign));
    verdict.evidence = references;
    return verdict;
}

CognitionReport AggregateBoundCognition(
    const std::vector<CognitionCase>& cases, const std::vector<CampaignManifest>& campaigns, const std::vector<CognitionVerdict>& runs)
{
    std::map<std::uint64_t, std::string> identities;
    std::vector<std::uint64_t> seeds;
    std::vector<std::string> errors;
    for (const auto& campaign : campaigns)
    {
        std::string error;
        if (!campaigns.empty())
        {
            auto cohort = campaign;
            cohort.seed = campaigns.front().seed;
            if (!CampaignIdentityMismatches(campaigns.front(), cohort).empty())
                errors.push_back("Expected seeds belong to different campaign cohorts.");
        }
        if (!ValidateCampaignManifest(campaign, error) ||
            !identities.emplace(campaign.seed, audit::ContentDigest(SerializeCampaignManifest(campaign))).second)
            errors.push_back("Invalid or duplicate expected campaign identity.");
        else
            seeds.push_back(campaign.seed);
    }
    auto admitted = runs;
    for (auto& run : admitted)
    {
        const auto found = identities.find(run.seed);
        if (!errors.empty() || found == identities.end() || run.campaignDigest != found->second || run.evidence.empty())
        {
            run.bindingVerified = false;
            run.semanticPassed.reset();
            run.personalityPassed.reset();
            run.diagnostic = "Sample belongs to a different or unbound campaign.";
        }
    }
    auto report = AggregateCognition(cases, seeds, admitted);
    report.errors.insert(report.errors.end(), errors.begin(), errors.end());
    return report;
}

bool ApplyCognitionReview(CognitionVerdict& verdict, const CognitionReview& review, std::string& error)
{
    if (!verdict.available || !verdict.bindingVerified)
        error = "Review cannot approve unavailable or unbound output.";
    else if (review.caseId != verdict.caseId || review.seed != verdict.seed)
        error = "Review case or seed differs.";
    else if (review.outputDigest != verdict.outputDigest)
        error = "Review outputDigest differs.";
    else if (review.sourceDigest != verdict.sourceDigest)
        error = "Review sourceDigest differs.";
    else if (review.reviewerId.empty() || review.reviewerId.starts_with("model:") || review.reviewerId.starts_with("deterministic:"))
        error = "An identified independent human reviewer is required.";
    else if (review.criterionDigest != verdict.criterionDigest)
        error = "Review criterionDigest differs.";
    else if (review.campaignDigest != verdict.campaignDigest)
        error = "Review campaignDigest differs.";
    else if (verdict.oraclePassed == false && review.semanticPassed == true)
        error = "A human review cannot erase a deterministic contradiction.";
    else if (!review.semanticPassed && !review.personalityPassed)
        error = "Review contains no judged criteria.";
    else
    {
        if (review.semanticPassed)
            verdict.semanticPassed = review.semanticPassed;
        if (review.personalityPassed)
            verdict.personalityPassed = review.personalityPassed;
        verdict.reviewerId = review.reviewerId;
        error.clear();
        return true;
    }
    return false;
}

CognitionReport AggregateCognition(
    const std::vector<CognitionCase>& cases, const std::vector<std::uint64_t>& seeds, const std::vector<CognitionVerdict>& runs)
{
    CognitionReport report;
    std::map<std::string, std::pair<std::string, std::string>> expected;
    for (const auto& item : cases)
        if (item.id.empty() || !expected.emplace(item.id, std::pair{item.sourceDigest, CognitionCriterionDigest(item)}).second)
            report.errors.push_back("Duplicate or empty frozen case ID: " + item.id);
    std::set<std::uint64_t> uniqueSeeds(seeds.begin(), seeds.end());
    if (seeds.empty() || uniqueSeeds.size() != seeds.size())
        report.errors.push_back("Empty or duplicate frozen seeds.");
    report.uniqueCases = expected.size();
    report.expectedRuns = expected.size() * uniqueSeeds.size();
    std::set<std::pair<std::string, std::uint64_t>> observed;
    for (const auto& run : runs)
    {
        const auto found = expected.find(run.caseId);
        if (found == expected.end() || !uniqueSeeds.contains(run.seed))
        {
            report.errors.push_back("Unexpected case/seed: " + run.caseId);
            continue;
        }
        if (!observed.emplace(run.caseId, run.seed).second)
        {
            report.errors.push_back("Duplicate case/seed: " + run.caseId);
            continue;
        }
        ++report.recordedRuns;
        if (!run.available)
            ++report.unavailable;
        const bool bound = run.bindingVerified && found->second.first == run.sourceDigest && found->second.second == run.criterionDigest &&
                           IsDigest(run.outputDigest);
        if (!bound)
            ++report.invalidBindings;
        const bool admitted = run.available && bound;
        if (admitted && run.mechanicalPassed)
            ++report.mechanicalPassed;
        if (admitted && run.semanticPassed && !run.reviewerId.empty())
            *run.semanticPassed ? ++report.semanticPassed : ++report.semanticFailed;
        else
            ++report.semanticUnjudged;
        if (admitted && run.personalityPassed && !run.reviewerId.empty())
            *run.personalityPassed ? ++report.personalityPassed : ++report.personalityFailed;
        else
            ++report.personalityUnjudged;
    }
    report.missingRuns = report.expectedRuns - report.recordedRuns;
    return report;
}

bool LoadCognitionCorpus(
    const std::filesystem::path& path, std::vector<CognitionCase>& cases, std::vector<std::uint64_t>& seeds, std::string& error)
{
    try
    {
        std::ifstream input(path, std::ios::binary);
        if (!input || std::filesystem::file_size(path) > 8 * 1024 * 1024)
            throw std::runtime_error("Cognition corpus unavailable or exceeds its byte limit.");
        const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const Json source = ParseOracleJson(bytes, 8 * 1024 * 1024);
        if (!source.at("schemaVersion").is_number_unsigned() || source.at("schemaVersion") != 1 || !source.at("cases").is_array())
            throw std::runtime_error("Unsupported cognition corpus schema.");
        std::vector<CognitionCase> loaded;
        for (const auto& seed : source.at("seeds"))
            if (!seed.is_number_unsigned())
                throw std::runtime_error("Frozen seeds must be nonnegative integers.");
        const auto declaredSeeds = source.at("seeds").get<std::vector<std::uint64_t>>();
        if (declaredSeeds.empty() || declaredSeeds.size() > 16 ||
            std::set<std::uint64_t>(declaredSeeds.begin(), declaredSeeds.end()).size() != declaredSeeds.size())
            throw std::runtime_error("Invalid frozen seeds.");
        std::set<std::string> ids;
        for (const auto& value : source.at("cases"))
        {
            CognitionCase item;
            item.id = value.at("id").get<std::string>();
            item.family = value.at("family").get<std::string>();
            item.oracleId = value.at("oracleId").get<std::string>();
            item.expectedJson = value.at("expected").dump();
            item.entityIds = value.value("entityIds", std::vector<std::string>{});
            item.requiredClaims = value.value("requiredClaims", std::vector<std::string>{});
            item.forbiddenClaims = value.value("forbiddenClaims", std::vector<std::string>{});
            item.uncertaintyDisposition = value.value("uncertaintyDisposition", std::string());
            item.conversation.id = item.id;
            item.conversation.title = value.value("title", item.family);
            for (const auto& turn : value.at("turns"))
                item.conversation.turns.push_back({turn.at("input").get<std::string>(), {{CheckKind::NotEmpty, {}, 0}}});
            if (item.id.empty() || item.family.empty() || item.oracleId.empty() || item.conversation.turns.empty() ||
                item.conversation.turns.size() > 24 || !ids.insert(item.id).second)
                throw std::runtime_error("Invalid or duplicate cognition case.");
            item.sourceDigest = CognitionSourceDigest(item.conversation);
            if (value.contains("sourceDigest") && value.at("sourceDigest") != item.sourceDigest)
                throw std::runtime_error("Frozen source digest differs.");
            loaded.push_back(std::move(item));
        }
        if (loaded.empty() || loaded.size() > 4096)
            throw std::runtime_error("Invalid cognition case count.");
        cases = std::move(loaded);
        seeds = declaredSeeds;
        error.clear();
        return true;
    }
    catch (const std::exception& failure)
    {
        error = failure.what();
        return false;
    }
}

bool ValidateCognitionPartitions(const std::vector<CognitionCase>& development, const std::vector<CognitionCase>& calibration,
    const std::vector<CognitionCase>& heldout, std::string& error)
{
    std::map<std::string, int> ids, families, sources, entities;
    int split = 0;
    for (const auto* partition : {&development, &calibration, &heldout})
    {
        for (const auto& item : *partition)
        {
            for (const auto& entity : item.entityIds)
            {
                const auto [found, inserted] = entities.emplace(entity, split);
                if (!inserted && found->second != split)
                {
                    error = "Cognition partitions share an entity: " + entity;
                    return false;
                }
            }
            for (auto [key, map] : {std::pair{item.id, &ids}, std::pair{item.family, &families}, std::pair{item.sourceDigest, &sources}})
            {
                const auto [found, inserted] = map->emplace(key, split);
                if (!inserted && found->second != split)
                {
                    error = "Cognition partitions share an ID, template family or source: " + key;
                    return false;
                }
            }
        }
        ++split;
    }
    error.clear();
    return true;
}

std::string CognitionReportJson(const CognitionReport& report, const std::vector<CognitionVerdict>& runs)
{
    Json results = Json::array();
    for (const auto& run : runs)
    {
        Json evidence = Json::array();
        for (const auto& reference : run.evidence)
        {
            std::string bytes;
            if (core::SerializeEvidenceRef(reference, bytes))
                evidence.push_back(Json::parse(bytes));
        }
        results.push_back({{"caseId", run.caseId}, {"seed", run.seed}, {"available", run.available},
            {"mechanicalPassed", run.mechanicalPassed}, {"bindingVerified", run.bindingVerified},
            {"semanticPassed", OptionalVerdict(run.semanticPassed)}, {"oraclePassed", OptionalVerdict(run.oraclePassed)},
            {"personalityPassed", OptionalVerdict(run.personalityPassed)}, {"reviewerId", run.reviewerId},
            {"outputDigest", run.outputDigest}, {"sourceDigest", run.sourceDigest}, {"criterionDigest", run.criterionDigest},
            {"campaignDigest", run.campaignDigest}, {"evidence", evidence}, {"diagnostic", run.diagnostic}});
    }
    return Json{{"schemaVersion", 1}, {"uniqueCases", report.uniqueCases}, {"expectedRuns", report.expectedRuns},
        {"recordedRuns", report.recordedRuns}, {"missingRuns", report.missingRuns}, {"unavailable", report.unavailable},
        {"invalidBindings", report.invalidBindings}, {"mechanicalPassed", report.mechanicalPassed},
        {"semanticPassed", report.semanticPassed}, {"semanticFailed", report.semanticFailed}, {"semanticUnjudged", report.semanticUnjudged},
        {"personalityPassed", report.personalityPassed}, {"personalityFailed", report.personalityFailed},
        {"personalityUnjudged", report.personalityUnjudged}, {"liveQualified", report.liveQualified}, {"errors", report.errors},
        {"runs", results}}
        .dump(2);
}
}
