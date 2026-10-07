#pragma once

#include "Evaluation/cognitionEvaluation.h"
#include "Evaluation/campaignManifest.h"
#include "Audit/contentDigest.h"
#include "observedLocalModel.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <nlohmann/json.hpp>
#include <utility>

namespace revia::quality
{

inline void RetainCognitionEvidenceOnce(const std::filesystem::path& path, const std::string& bytes)
{
    std::string error;
    if (!evaluation::WriteEvaluationArtifactOnce(path, bytes, error))
        throw std::runtime_error(error);
}

inline void RetainCognitionEvidence(const std::filesystem::path& path, const std::string& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << bytes;
    file.flush();
    file.close();
    if (!file.good())
    {
        throw std::runtime_error("Cannot retain the actual cognition evidence.");
    }
}

inline nlohmann::json PendingCognitionReview(const evaluation::CognitionCase& item, const evaluation::CaseOutcome& output,
    const evaluation::CognitionVerdict& verdict, std::size_t firstRequest, std::size_t endRequest)
{
    nlohmann::json turns = nlohmann::json::array();
    for (const auto& turn : output.turns)
    {
        turns.push_back({{"input", turn.input}, {"reply", turn.reply}, {"rawReply", turn.rawReply}, {"modelSucceeded", turn.modelSucceeded},
            {"failures", turn.failures}});
    }
    return {{"caseId", verdict.caseId}, {"seed", verdict.seed}, {"outputDigest", verdict.outputDigest},
        {"sourceDigest", verdict.sourceDigest}, {"criterionDigest", verdict.criterionDigest}, {"family", item.family},
        {"oracleId", item.oracleId}, {"expected", nlohmann::json::parse(item.expectedJson)}, {"requiredClaims", item.requiredClaims},
        {"forbiddenClaims", item.forbiddenClaims}, {"uncertaintyDisposition", item.uncertaintyDisposition}, {"entityIds", item.entityIds},
        {"available", verdict.available}, {"bindingVerified", verdict.bindingVerified}, {"mechanicalPassed", verdict.mechanicalPassed},
        {"deterministicSemanticVerdict", verdict.semanticPassed ? nlohmann::json(*verdict.semanticPassed) : nlohmann::json(nullptr)},
        {"semanticPassed", nullptr}, {"personalityPassed", nullptr}, {"reviewerId", ""}, {"reviewStatus", "pending"},
        {"firstRequestIndex", firstRequest}, {"endRequestIndexExclusive", endRequest}, {"turns", turns}};
}

inline std::uint64_t CognitionObservationTime()
{
    const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (time <= 0)
        throw std::runtime_error("The host observation clock is unavailable.");
    return static_cast<std::uint64_t>(time);
}

inline evaluation::CognitionVerdict EvaluateRetainedCognition(const evaluation::CognitionCase& item, const evaluation::CaseOutcome& outcome,
    const evaluation::CampaignManifest& campaign, const std::string& casePath, const std::string& sourceBytes,
    const std::string& outputBytes, std::uint64_t sourceTime, std::uint64_t outputTime, const evaluation::OracleRegistry& oracles)
{
    core::TaskContract task;
    task.stamp = {"cognition-evaluation", campaign.campaignId, 1, item.id, std::to_string(campaign.seed), 1};
    task.scope.companionId = task.stamp.companionId;
    task.scope.audience = {identity::AudienceKind::Unknown, "cognition-evaluation", 1, {}};
    task.cancellation.origin = task.stamp;
    task.goal = "Evaluate frozen cognition case " + item.id + " against its retained source and actual output";
    task.positiveConstraints = {"Bind the exact retained bytes to the immutable campaign and case identity"};
    task.negativeConstraints = {"Do not substitute source, output, provider or build identity", "Do not invent independent review"};
    task.deliverables = {"Scoped cognition verdict with retained source and output evidence"};
    task.acceptanceObligations = {"Admit campaign, task and evidence before applying the frozen oracle"};
    task.sourceKind = "cognition-evaluation";
    task.sourceId = item.id;
    core::EvidenceRef source;
    source.id = item.id + "/" + std::to_string(campaign.seed) + "/source";
    source.sourceLocator = std::filesystem::absolute(casePath + ".source.txt").generic_string();
    source.digest = audit::ContentDigest(sourceBytes);
    source.mediaType = "text/plain";
    source.stamp = task.stamp;
    source.scope = task.scope;
    source.observedAtUnixMs = sourceTime;
    source.sourceId = "cognition-source";
    auto output = source;
    output.id = item.id + "/" + std::to_string(campaign.seed) + "/output";
    output.sourceLocator = std::filesystem::absolute(casePath + ".outcome.json").generic_string();
    output.digest = audit::ContentDigest(outputBytes);
    output.mediaType = "application/json";
    output.observedAtUnixMs = outputTime;
    output.sourceId = "cognition-output";
    core::ContractValidation validation;
    const auto bundle = core::EvidenceBundle::Create({source, output}, task.stamp, task.scope, validation);
    if (!bundle)
        throw std::runtime_error("Cannot construct cognition evidence bundle: " + validation.code);
    std::string serialized;
    validation = core::SerializeTaskContract(task, serialized);
    if (!validation)
        throw std::runtime_error("Cannot retain cognition task contract: " + validation.code);
    RetainCognitionEvidenceOnce(casePath + ".task.json", serialized);
    validation = core::SerializeEvidenceBundle(*bundle, serialized);
    if (!validation)
        throw std::runtime_error("Cannot retain cognition evidence bundle: " + validation.code);
    RetainCognitionEvidenceOnce(casePath + ".bundle.json", serialized);
    const auto current = task.stamp;
    const auto guard = [current](const runtime::RuntimeStamp& candidate) { return core::SameRuntimeStamp(candidate, current); };
    return evaluation::EvaluateBoundCognitionCase(item, outcome, campaign, *bundle, task, current, task.scope, guard, oracles);
}

inline int RunCognitionLive(const std::vector<evaluation::CognitionCase>& cases, const std::vector<std::uint64_t>& seeds,
    const std::filesystem::path& output, const evaluation::ConversationEvaluator::TurnRunner& runner, const std::string& modelName,
    ObservedLocalModel& observed, std::optional<evaluation::CampaignManifest> base = std::nullopt, bool fixtureOnly = false)
{
    if (!base && !fixtureOnly)
        throw std::runtime_error("Live cognition evaluation requires an immutable campaign manifest.");
    std::vector<evaluation::CampaignManifest> campaigns;
    if (base)
    {
        for (const auto seed : seeds)
        {
            auto campaign = *base;
            campaign.seed = seed;
            std::string error;
            if (!evaluation::ValidateCampaignManifest(campaign, error))
                throw std::runtime_error("Cannot admit cognition campaign: " + error);
            campaigns.push_back(std::move(campaign));
        }
    }
    evaluation::OracleRegistry oracles;
    std::vector<evaluation::CognitionVerdict> runs;
    nlohmann::json seedArtifacts = nlohmann::json::array();
    for (const auto seed : seeds)
    {
        const auto seedPath = std::filesystem::path(output.string() + ".seed-" + std::to_string(seed) + ".jsonl");
        if (std::filesystem::exists(seedPath) || std::filesystem::exists(seedPath.string() + ".requests.json") ||
            std::filesystem::exists(seedPath.string() + ".responses.json") || std::filesystem::exists(seedPath.string() + ".review.json"))
        {
            throw std::runtime_error("Retained cognition seed output already exists; choose a fresh report path.");
        }
        observed.SetCompletionSeed(seed);
        const auto campaign = base ? std::optional<evaluation::CampaignManifest>(campaigns.at(seedArtifacts.size())) : std::nullopt;
        if (campaign)
        {
            std::string error;
            if (!evaluation::WriteCampaignManifestOnce(seedPath.string() + ".campaign.json", *campaign, error))
                throw std::runtime_error(error);
        }
        const auto firstSeedRequest = observed.Requests().size();
        evaluation::EvaluationReport report;
        report.modelName = modelName;
        nlohmann::json pending = nlohmann::json::array();
        for (const auto& item : cases)
        {
            const auto casePath = seedPath.string() + ".case-" + std::to_string(report.cases.size() + 1);
            const auto sourceBytes = evaluation::CognitionSourceBytes(item.conversation);
            const auto sourceTime = CognitionObservationTime();
            RetainCognitionEvidenceOnce(casePath + ".source.txt", sourceBytes);
            const auto firstRequest = observed.Requests().size();
            evaluation::EvaluationReport attempt;
            try
            {
                attempt = evaluation::ConversationEvaluator::Run({item.conversation}, runner, modelName);
            }
            catch (const std::exception& exception)
            {
                evaluation::CaseOutcome unavailable;
                unavailable.id = item.id;
                unavailable.title = item.conversation.title;
                unavailable.clause = item.conversation.clause;
                unavailable.unavailable = true;
                for (const auto& turn : item.conversation.turns)
                {
                    evaluation::TurnOutcome failed;
                    failed.input = turn.input;
                    failed.failures.push_back(exception.what());
                    unavailable.turns.push_back(std::move(failed));
                }
                attempt.cases.push_back(std::move(unavailable));
                attempt.unavailable = 1;
            }
            if (attempt.cases.size() != 1)
            {
                throw std::runtime_error("Cognition evaluator returned a different case count.");
            }
            const auto endRequest = observed.Requests().size();
            const auto outputBytes = evaluation::CognitionOutputBytes(attempt.cases.front());
            const auto outputTime = CognitionObservationTime();
            RetainCognitionEvidenceOnce(casePath + ".outcome.json", outputBytes);
            const auto verdict = campaign ? EvaluateRetainedCognition(item, attempt.cases.front(), *campaign, casePath, sourceBytes,
                                                outputBytes, sourceTime, outputTime, oracles)
                                          : evaluation::EvaluateCognitionCase(item, attempt.cases.front(), seed, oracles);
            pending.push_back(PendingCognitionReview(item, attempt.cases.front(), verdict, firstRequest, endRequest));
            pending.back()["campaignDigest"] = verdict.campaignDigest;
            pending.back()["evidenceBundle"] = campaign ? nlohmann::json(casePath + ".bundle.json") : nlohmann::json(nullptr);
            runs.push_back(verdict);
            if (report.startedAt.empty())
            {
                report.startedAt = attempt.startedAt;
            }
            report.elapsedMilliseconds += attempt.elapsedMilliseconds;
            report.passed += attempt.passed;
            report.failed += attempt.failed;
            report.unavailable += attempt.unavailable;
            report.repairedTurns += attempt.repairedTurns;
            report.stopped = report.stopped || attempt.stopped;
            RetainCognitionEvidenceOnce(casePath + ".jsonl", attempt.ToJsonLines());
            RetainCognitionEvidenceOnce(casePath + ".review.json", pending.back().dump(2) + '\n');
            observed.SaveTrafficSince(casePath, firstRequest);
            report.cases.push_back(std::move(attempt.cases.front()));
            auto progress =
                base ? evaluation::AggregateBoundCognition(cases, campaigns, runs) : evaluation::AggregateCognition(cases, seeds, runs);
            progress.liveQualified = false;
            // This disposable progress summary is rebuilt from the immutable per-case artifacts.
            RetainCognitionEvidence(output.string() + ".progress.json", evaluation::CognitionReportJson(progress, runs) + '\n');
            std::cout << "Cognition seed " << seed << " case " << report.cases.size() << '/' << cases.size() << ' ' << item.id << ": "
                      << (verdict.available ? "recorded" : "unavailable") << '\n'
                      << std::flush;
        }
        RetainCognitionEvidenceOnce(seedPath, report.ToJsonLines());
        observed.SaveTrafficSince(seedPath, firstSeedRequest);
        const auto seedRequests = observed.Requests();
        const bool injected =
            seedRequests.size() > firstSeedRequest &&
            std::all_of(seedRequests.begin() + static_cast<std::ptrdiff_t>(firstSeedRequest), seedRequests.end(),
                [seed](const auto& request) { return request.is_object() && request.contains("seed") && request.at("seed") == seed; });
        RetainCognitionEvidenceOnce(seedPath.string() + ".review.json",
            nlohmann::json{{"schemaVersion", 1}, {"model", modelName}, {"seed", seed}, {"independentReview", "pending"},
                {"backendSeedVerified", false}, {"requests", seedPath.string() + ".requests.json"},
                {"responses", seedPath.string() + ".responses.json"}, {"runs", pending}}
                    .dump(2) +
                '\n');
        seedArtifacts.push_back({{"seed", seed}, {"rawReport", seedPath.string()}, {"pendingReview", seedPath.string() + ".review.json"},
            {"requests", seedPath.string() + ".requests.json"}, {"responses", seedPath.string() + ".responses.json"},
            {"completionRequestSeedInjected", injected}, {"backendSeedVerified", false},
            {"campaign", campaign ? nlohmann::json(seedPath.string() + ".campaign.json") : nlohmann::json(nullptr)}});
    }
    observed.SetCompletionSeed(std::nullopt);
    auto aggregate =
        base ? evaluation::AggregateBoundCognition(cases, campaigns, runs) : evaluation::AggregateCognition(cases, seeds, runs);
    aggregate.liveQualified = false;
    auto retained = nlohmann::json::parse(evaluation::CognitionReportJson(aggregate, runs));
    retained["seedArtifacts"] = std::move(seedArtifacts);
    retained["independentPersonalityReview"] = "pending";
    retained["fixtureOnly"] = fixtureOnly;
    retained["timingBoundary"] = "existing-conversation-evaluator-per-case-wall-time";
    RetainCognitionEvidenceOnce(output, retained.dump(2) + '\n');
    return aggregate.unavailable > 0 || aggregate.missingRuns > 0 || aggregate.invalidBindings > 0 || !aggregate.errors.empty() ? 2 : 0;
}

}
