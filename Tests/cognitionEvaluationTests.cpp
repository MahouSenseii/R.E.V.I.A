#include "Evaluation/cognitionEvaluation.h"
#include "Audit/contentDigest.h"

#include <iostream>
#include <fstream>
#include <set>
#include <stdexcept>

namespace
{
using namespace revia::evaluation;

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

CognitionCase MakeCase(int index)
{
    CognitionCase result;
    result.id = "frozen-" + std::to_string(index);
    result.family = "fixture-family-" + std::to_string(index % 12);
    result.oracleId = "json-exact-v1";
    result.conversation.id = result.id;
    result.conversation.title = "Supplied facts";
    result.conversation.turns.push_back(
        {"The build produces " + std::to_string(index + 10) + " objects. Return JSON containing objects.", {{CheckKind::NotEmpty, {}, 0}}});
    result.expectedJson = "{\"objects\":" + std::to_string(index + 10) + "}";
    result.sourceDigest = revia::audit::ContentDigest(result.conversation.turns.front().input);
    return result;
}

CaseOutcome Output(const CognitionCase& item, const std::string& answer)
{
    CaseOutcome result;
    result.id = item.id;
    result.turns.push_back({item.conversation.turns.front().input, answer, answer, true, false, {}});
    return result;
}

void TestWrongAndUnreviewed()
{
    OracleRegistry oracles;
    for (int index = 0; index < 30; ++index)
    {
        auto item = MakeCase(index);
        const auto wrong = EvaluateCognitionCase(item, Output(item, "{\"objects\":9999}"), 11, oracles);
        Check(wrong.available && wrong.mechanicalPassed, "Wrong fixture did not mechanically pass.");
        Check(wrong.semanticPassed == false, "Fluent wrong answer was a semantic pass.");
        Check(!wrong.personalityPassed.has_value(), "Deterministic truth invented personality approval.");
        item.oracleId = "human-rubric-v1";
        const auto unreviewed = EvaluateCognitionCase(item, Output(item, item.expectedJson), 11, oracles);
        Check(!unreviewed.semanticPassed.has_value(), "Unreviewed answer was accepted.");
        CognitionReview badReview{item.id, 11, "owner", "incorrect-output-digest", item.sourceDigest, true, true};
        std::string error;
        auto changed = unreviewed;
        Check(!ApplyCognitionReview(changed, badReview, error), "Mismatched output review was admitted.");
        badReview.outputDigest = changed.outputDigest;
        badReview.sourceDigest = "changed-source";
        Check(!ApplyCognitionReview(changed, badReview, error), "Mismatched source review was admitted.");
        badReview.sourceDigest = changed.sourceDigest;
        badReview.criterionDigest = changed.criterionDigest;
        badReview.reviewerId = "";
        Check(!ApplyCognitionReview(changed, badReview, error), "Anonymous semantic review was admitted.");
    }
    auto item = MakeCase(0);
    auto substituted = Output(item, item.expectedJson);
    substituted.turns[0].input = "Other source";
    Check(!EvaluateCognitionCase(item, substituted, 11, oracles).available, "A different input was admitted under a frozen case ID.");
    const auto extraClaim = EvaluateCognitionCase(item, Output(item, "{\"objects\":10,\"success\":true}"), 11, oracles);
    Check(extraClaim.semanticPassed == false, "Unsupported extra claim was accepted.");
    const auto contradicted = EvaluateCognitionCase(item, Output(item, "Not true: {\"objects\":10}"), 11, oracles);
    Check(contradicted.semanticPassed == false, "Substring truth accepted a contradiction.");
    const auto duplicate = EvaluateCognitionCase(item, Output(item, "{\"objects\":9999,\"objects\":10}"), 11, oracles);
    Check(duplicate.semanticPassed == false, "Duplicate JSON keys discarded an incorrect claim.");
    Check(EvaluateCognitionCase(item, Output(item, "{\"objects\":10.0}"), 11, oracles).semanticPassed == false,
        "A non-integer claim passed an integer criterion.");
    auto knownWrong = extraClaim;
    CognitionReview forged{item.id, 11, "reviewer", knownWrong.outputDigest, knownWrong.sourceDigest, true, std::nullopt};
    forged.criterionDigest = knownWrong.criterionDigest;
    std::string error;
    Check(!ApplyCognitionReview(knownWrong, forged, error), "Review erased a deterministic contradiction.");
    forged.semanticPassed.reset();
    forged.personalityPassed = true;
    Check(ApplyCognitionReview(knownWrong, forged, error), "Bound personality-only review was rejected.");
    forged.semanticPassed = true;
    Check(!ApplyCognitionReview(knownWrong, forged, error), "Personality review erased deterministic oracle provenance.");
}

void TestCampaignEvidenceAdmission()
{
    auto item = MakeCase(0);
    const auto output = Output(item, item.expectedJson);
    CampaignManifest campaign;
    campaign.campaignId = "independent-campaign";
    campaign.commit = std::string(40, 'a');
    campaign.sourceDigest = std::string(64, 'b');
    campaign.buildDigest = std::string(64, 'c');
    campaign.providerDigest = std::string(64, 'd');
    campaign.providerAvailable = true;
    campaign.settingsDigest = std::string(64, 'e');
    campaign.fixtureDigest = std::string(64, 'f');
    campaign.oracleVersion = "json-exact-v1";
    campaign.hardware = "fixture-only";
    campaign.seed = 11;
    campaign.timingBoundary = "fixture";
    revia::core::TaskContract task;
    task.stamp = {"evaluation", campaign.campaignId, 1, item.id, "11", 1};
    task.scope.companionId = "evaluation";
    task.scope.audience = {revia::identity::AudienceKind::Unknown, "cognition-evaluation", 1, {}};
    task.cancellation.origin = task.stamp;
    task.goal = "Measure the frozen answer independently";
    task.deliverables = {"Bound cognition verdict"};
    task.acceptanceObligations = {"Bind source, output and campaign before judging"};
    task.sourceKind = "cognition-evaluation";
    task.sourceId = item.id;
    revia::core::EvidenceRef source;
    source.id = "source";
    source.sourceLocator = "fixture://source";
    source.digest = item.sourceDigest;
    source.mediaType = "text/plain";
    source.stamp = task.stamp;
    source.scope = task.scope;
    source.observedAtUnixMs = 1000;
    source.sourceId = "cognition-source";
    auto result = source;
    result.id = "output";
    result.sourceLocator = "fixture://output";
    result.digest = revia::audit::ContentDigest(CognitionOutputBytes(output));
    result.sourceId = "cognition-output";
    revia::core::ContractValidation validation;
    auto bundle = revia::core::EvidenceBundle::Create({source, result}, task.stamp, task.scope, validation);
    Check(bundle.has_value(), "Campaign fixture bundle invalid.");
    const auto guard = [&task](const auto& stamp) { return revia::core::SameRuntimeStamp(stamp, task.stamp); };
    OracleRegistry oracles;
    auto valid = EvaluateBoundCognitionCase(item, output, campaign, *bundle, task, task.stamp, task.scope, guard, oracles);
    Check(valid.bindingVerified && valid.semanticPassed == true, "Bound campaign control failed.");
    CognitionReview review{item.id, 11, "independent-reviewer", valid.outputDigest, valid.sourceDigest, std::nullopt, true};
    review.criterionDigest = valid.criterionDigest;
    review.campaignDigest = "different-campaign";
    std::string error;
    Check(!ApplyCognitionReview(valid, review, error), "Foreign campaign review was admitted.");
    auto swapped = campaign;
    swapped.buildDigest = std::string(64, '9');
    auto report = AggregateBoundCognition({item}, {swapped}, {valid});
    Check(report.expectedRuns == 1 && report.recordedRuns == 1 && report.invalidBindings == 1 && report.semanticPassed == 0,
        "Different build entered the live semantic numerator.");
    swapped = campaign;
    swapped.providerDigest = std::string(64, '8');
    Check(AggregateBoundCognition({item}, {swapped}, {valid}).semanticPassed == 0, "Different provider entered the live numerator.");
    swapped.seed = 29;
    const auto mixed = AggregateBoundCognition({item}, {campaign, swapped}, {valid});
    Check(!mixed.errors.empty() && mixed.semanticPassed == 0 && mixed.expectedRuns == 2,
        "Different expected providers across seeds entered one cohort.");
    auto foreignScope = task.scope;
    foreignScope.audience.audienceId = "another-audience";
    auto refused = EvaluateBoundCognitionCase(item, output, campaign, *bundle, task, task.stamp, foreignScope, guard, oracles);
    Check(!refused.bindingVerified && !refused.semanticPassed, "Cross-audience evidence entered the semantic numerator.");
    result.digest = std::string(64, '7');
    const auto wrongOutput = revia::core::EvidenceBundle::Create({source, result}, task.stamp, task.scope, validation);
    refused = EvaluateBoundCognitionCase(item, output, campaign, *wrongOutput, task, task.stamp, task.scope, guard, oracles);
    Check(!refused.bindingVerified && !refused.semanticPassed, "Different output evidence entered the semantic numerator.");
    std::stop_source stop;
    stop.request_stop();
    refused =
        EvaluateBoundCognitionCase(item, output, campaign, *wrongOutput, task, task.stamp, task.scope, guard, oracles, stop.get_token());
    Check(!refused.bindingVerified, "Cancelled task admitted campaign evidence.");
}

void TestFrozenDenominators()
{
    std::vector<CognitionCase> cases;
    std::vector<CognitionVerdict> runs;
    const std::vector<std::uint64_t> seeds{11, 29, 47};
    OracleRegistry oracles;
    for (int index = 0; index < 240; ++index)
    {
        cases.push_back(MakeCase(index));
        for (auto seed : seeds)
            runs.push_back(EvaluateCognitionCase(cases.back(), Output(cases.back(), cases.back().expectedJson), seed, oracles));
    }
    const auto complete = AggregateCognition(cases, seeds, runs);
    Check(complete.expectedRuns == 720 && complete.uniqueCases == 240 && complete.semanticPassed == 720,
        "Complete fixture denominator differs from the frozen manifest.");
    Check(!complete.liveQualified && complete.personalityUnjudged == 720, "Fixture aggregation claimed a live/personality qualification.");
    runs[7].available = false;
    runs[7].semanticPassed.reset();
    runs[7].diagnostic = "Cancelled";
    runs[19].bindingVerified = false;
    runs[19].diagnostic = "Source digest mismatch";
    const auto interrupted = AggregateCognition(cases, seeds, runs);
    Check(interrupted.expectedRuns == 720 && interrupted.recordedRuns == 720 && interrupted.uniqueCases == 240,
        "Unavailable evidence disappeared from the denominator.");
    Check(interrupted.unavailable == 1 && interrupted.invalidBindings == 1 && interrupted.semanticPassed == 718,
        "Cancelled or mismatched evidence entered the pass numerator.");
    runs.pop_back();
    Check(AggregateCognition(cases, seeds, runs).missingRuns == 1, "Missing run was silently replaced or omitted.");
    runs.push_back(runs.front());
    Check(!AggregateCognition(cases, seeds, runs).errors.empty(), "Duplicate case/seed aggregation was accepted.");
    runs.back().caseId = "development-case";
    Check(!AggregateCognition(cases, seeds, runs).errors.empty(), "Development case filled a held-out slot.");
    runs.back().caseId = cases.back().id;
    runs.back().seed = seeds.back();
    runs.back().criterionDigest = "changed-oracle-ground-truth";
    Check(AggregateCognition(cases, seeds, runs).invalidBindings >= 2, "Changed oracle criteria were admitted.");
}

void TestCorpusPartitions(const std::filesystem::path& directory)
{
    std::vector<CognitionCase> development, calibration, heldout;
    std::vector<std::uint64_t> seeds;
    std::string error;
    Check(LoadCognitionCorpus(directory / "development.json", development, seeds, error), "Development corpus could not be loaded.");
    Check(LoadCognitionCorpus(directory / "calibration.json", calibration, seeds, error), "Calibration corpus could not be loaded.");
    Check(LoadCognitionCorpus(directory / "heldout-manifest.json", heldout, seeds, error), "Frozen corpus could not be loaded.");
    Check(heldout.size() == 240 && seeds == std::vector<std::uint64_t>{11, 29, 47}, "Frozen corpus case/seed denominator changed.");
    std::set<std::string> families;
    for (const auto& item : heldout)
        families.insert(item.family);
    Check(families.size() == 12, "Frozen corpus family count changed.");
    Check(ValidateCognitionPartitions(development, calibration, heldout, error), "Frozen partitions overlap.");
    auto leaked = heldout;
    leaked.front().family = development.front().family;
    Check(!ValidateCognitionPartitions(development, calibration, leaked, error), "Development family leaked into held-out cases.");
    leaked = heldout;
    leaked.front().sourceDigest = calibration.front().sourceDigest;
    Check(!ValidateCognitionPartitions(development, calibration, leaked, error), "Calibration source leaked into held-out cases.");
    leaked = heldout;
    leaked.front().entityIds = development.front().entityIds;
    Check(!ValidateCognitionPartitions(development, calibration, leaked, error), "Development entities leaked into held-out cases.");
    const auto temporary = std::filesystem::temp_directory_path() / ("revia-cognition-duplicate-" + heldout.front().sourceDigest + ".json");
    {
        std::ofstream file(temporary);
        file << R"({"schemaVersion":2,"schemaVersion":1,"seeds":[11],"cases":[]})";
    }
    const auto original = heldout.size();
    Check(!LoadCognitionCorpus(temporary, heldout, seeds, error), "Duplicate corpus keys were silently discarded.");
    Check(heldout.size() == original, "Rejected corpus changed the frozen cases.");
    std::filesystem::remove(temporary);
}
}

int main(int argc, char** argv)
{
    try
    {
        const std::string selected = argc >= 2 ? argv[1] : "all";
        if (selected == "all" || selected == "A")
        {
            TestWrongAndUnreviewed();
            TestCampaignEvidenceAdmission();
        }
        if (selected == "all" || selected == "B")
        {
            TestFrozenDenominators();
            if (argc >= 3)
                TestCorpusPartitions(argv[2]);
        }
        if (selected != "all" && selected != "A" && selected != "B")
            throw std::runtime_error("Unknown test selector.");
        std::cout << "Cognition " << selected << " fixtures passed; live qualification is separate.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
