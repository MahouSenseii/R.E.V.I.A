#include "Evaluation/cognitionEvaluation.h"
#include "Audit/contentDigest.h"

#include <iostream>
#include <fstream>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>

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

nlohmann::json SerializedVerdict(const CognitionCase& item, const CognitionVerdict& verdict)
{
    return nlohmann::json::parse(CognitionReportJson(AggregateCognition({item}, {verdict.seed}, {verdict}), {verdict})).at("runs").front();
}

void TestStructuredDiagnostics()
{
    OracleRegistry oracles;
    const auto item = MakeCase(0);
    const auto verify = [&](const std::string& answer, const std::string& category, const nlohmann::json& values)
    {
        const auto verdict = EvaluateCognitionCase(item, Output(item, answer), 11, oracles);
        const auto retained = SerializedVerdict(item, verdict);
        Check(retained.contains("finalAnswerDiagnostic"), "Structured diagnostic metadata was not retained.");
        Check(retained.at("finalAnswerDiagnostic") == category, "Structured answer was assigned the wrong failure category.");
        Check(retained.at("answerValuesPassed") == values, "Uncomparable output invented a factual judgment.");
        Check(verdict.oraclePassed == (category == "match"), "Diagnostic classification changed the strict oracle verdict.");
        Check(!verdict.personalityPassed.has_value(), "Structured diagnostics invented personality approval.");
    };
    verify(R"({"objects":10})", "match", true);
    verify(R"({"objects":9999})", "value_mismatch", false);
    verify(R"({"objects":"10"})", "shape_mismatch", nullptr);
    verify(R"({"objects":10.0})", "shape_mismatch", nullptr);
    verify(R"({})", "shape_mismatch", nullptr);
    verify(R"({"objects":10,"extra":true})", "shape_mismatch", nullptr);
    verify("There are 10 objects.", "invalid_json", nullptr);
    verify("```json\n{\"objects\":10}\n```", "invalid_json", nullptr);
    verify(R"({"objects":10} But actually there are 9999.)", "invalid_json", nullptr);
    verify(R"({"objects":9999,"objects":10})", "invalid_json", nullptr);
    verify(std::string(33, '[') + "10" + std::string(33, ']'), "invalid_json", nullptr);
    verify(std::string(262144 - item.expectedJson.size(), ' ') + item.expectedJson, "match", true);
    verify(std::string(262145 - item.expectedJson.size(), ' ') + item.expectedJson, "invalid_json", nullptr);

    auto nested = item;
    nested.expectedJson = R"({"items":[{"count":-2,"missing":null}],"name":"box"})";
    const auto judgeNested = [&](const std::string& answer, const std::string& category)
    {
        const auto verdict = EvaluateCognitionCase(nested, Output(nested, answer), 11, oracles);
        Check(SerializedVerdict(nested, verdict).at("finalAnswerDiagnostic") == category,
            "Nested diagnostic lost a shape or value mismatch.");
    };
    judgeNested(R"({"name":"box","items":[{"missing":null,"count":-2}]})", "match");
    judgeNested(R"({"items":[{"count":2,"missing":null}],"name":"box"})", "value_mismatch");
    judgeNested(R"({"items":[{"count":-2,"missing":"unknown"}],"name":"box"})", "shape_mismatch");
    judgeNested(R"({"items":[],"name":"box"})", "shape_mismatch");
    judgeNested(R"({"items":[{"count":-2}],"name":"box"})", "shape_mismatch");
    judgeNested(R"({"items":[{"count":9999,"count":-2,"missing":null}],"name":"box"})", "invalid_json");

    auto introduced = Output(item, R"({"objects":11})");
    introduced.turns.back().rawReply = item.expectedJson;
    auto verdict = EvaluateCognitionCase(item, introduced, 11, oracles);
    auto retained = SerializedVerdict(item, verdict);
    Check(retained.at("rawAnswerDiagnostic") == "match" && retained.at("rawAnswerValuesPassed") == true &&
              retained.at("repairIntroducedFailure") == true && retained.at("repairRescuedAnswer") == false,
        "A correct raw answer corrupted before delivery was not identified.");
    auto rescued = Output(item, item.expectedJson);
    rescued.turns.back().rawReply = "There are 10 objects.";
    verdict = EvaluateCognitionCase(item, rescued, 11, oracles);
    retained = SerializedVerdict(item, verdict);
    Check(retained.at("rawAnswerDiagnostic") == "invalid_json" && retained.at("rawAnswerValuesPassed").is_null() &&
              retained.at("repairIntroducedFailure") == false && retained.at("repairRescuedAnswer") == true,
        "A strict failure rescued before delivery was not identified.");
    rescued.turns.back().rawReply.clear();
    retained = SerializedVerdict(item, EvaluateCognitionCase(item, rescued, 11, oracles));
    Check(retained.at("rawAnswerDiagnostic") == "unjudged" && retained.at("repairRescuedAnswer") == false,
        "An absent raw response invented a repair judgment.");
    auto human = item;
    human.oracleId = "human-rubric-v1";
    retained = SerializedVerdict(human, EvaluateCognitionCase(human, Output(human, item.expectedJson), 11, oracles));
    Check(retained.at("finalAnswerDiagnostic") == "unjudged" && retained.at("answerValuesPassed").is_null(),
        "A human-rubric case inherited an unrequested structured oracle.");
}

void TestDiagnosticAccounting()
{
    const auto item = MakeCase(0);
    OracleRegistry oracles;
    std::vector<CognitionVerdict> runs;
    const std::vector<std::uint64_t> seeds{11, 29, 47, 61, 73, 89};
    for (std::size_t index = 0; index < seeds.size(); ++index)
    {
        const std::vector<std::string> answers{item.expectedJson, "Correct prose: 10 objects.", R"({"objects":"10"})", R"({"objects":11})",
            item.expectedJson, item.expectedJson};
        runs.push_back(EvaluateCognitionCase(item, Output(item, answers[index]), seeds[index], oracles));
    }
    runs[4].available = false;
    runs[5].bindingVerified = false;
    auto report = nlohmann::json::parse(CognitionReportJson(AggregateCognition({item}, seeds, runs), runs));
    Check(report.at("expectedRuns") == 6 && report.at("recordedRuns") == 6 && report.at("unavailable") == 1 &&
              report.at("invalidBindings") == 1 && report.at("semanticPassed") == 1,
        "Diagnostic counters changed the frozen denominator or strict numerator.");
    Check(report.at("invalidJson") == 1 && report.at("shapeMismatch") == 1 && report.at("valueMismatch") == 1 &&
              report.at("structuredMatch") == 1 && report.at("structuredUnjudged") == 2,
        "Unavailable or unbound output entered a structured diagnostic numerator.");
    for (std::size_t index : {4u, 5u})
    {
        const auto& retained = report.at("runs").at(index);
        Check(retained.at("finalAnswerDiagnostic") == "unjudged" && retained.at("rawAnswerDiagnostic") == "unjudged" &&
                  retained.at("answerValuesPassed").is_null() && retained.at("rawAnswerValuesPassed").is_null(),
            "Serialized unavailable or unbound output retained a structured judgment.");
        Check(retained.at("semanticPassed").is_null() && retained.at("oraclePassed").is_null() &&
                  retained.at("personalityPassed").is_null() && retained.at("mechanicalPassed") == false && retained.at("reviewerId") == "",
            "Serialized unavailable or unbound output retained an earlier judgment.");
    }
    runs.pop_back();
    report = nlohmann::json::parse(CognitionReportJson(AggregateCognition({item}, seeds, runs), runs));
    Check(report.at("missingRuns") == 1 && report.at("expectedRuns") == 6 && report.at("structuredUnjudged") == 1,
        "A missing diagnostic slot was silently filled or removed from the denominator.");
    auto introduced = Output(item, R"({"objects":11})");
    introduced.turns.back().rawReply = item.expectedJson;
    auto rescued = Output(item, item.expectedJson);
    rescued.turns.back().rawReply = "Correct prose: 10 objects.";
    runs = {EvaluateCognitionCase(item, introduced, 11, oracles), EvaluateCognitionCase(item, rescued, 29, oracles)};
    report = nlohmann::json::parse(CognitionReportJson(AggregateCognition({item}, {11, 29}, runs), runs));
    Check(report.at("repairIntroducedFailures") == 1 && report.at("repairRescuedAnswers") == 1,
        "Raw/delivered repair transitions were not counted independently.");
    runs.front().bindingVerified = false;
    runs.back().available = false;
    report = nlohmann::json::parse(CognitionReportJson(AggregateCognition({item}, {11, 29}, runs), runs));
    Check(report.at("repairIntroducedFailures") == 0 && report.at("repairRescuedAnswers") == 0,
        "Unbound or unavailable repair metadata entered a diagnostic numerator.");
    Check(report.at("runs").front().at("repairIntroducedFailure") == false && report.at("runs").back().at("repairRescuedAnswer") == false,
        "Serialized unadmitted repair metadata invented an approved transition.");
    auto original = EvaluateCognitionCase(item, Output(item, item.expectedJson), 11, oracles);
    auto rejected = original;
    rejected.criterionDigest = std::string(64, 'e');
    report = nlohmann::json::parse(CognitionReportJson(AggregateCognition({item}, {11}, {rejected}), {rejected}));
    Check(report.at("invalidBindings") == 1 && report.at("runs").front().at("bindingVerified") == false &&
              report.at("runs").front().at("oraclePassed").is_null() && report.at("runs").front().at("finalAnswerDiagnostic") == "unjudged",
        "Changed frozen criteria retained a serialized judgment.");
    rejected = original;
    rejected.sourceDigest = std::string(64, 'f');
    report = nlohmann::json::parse(CognitionReportJson(AggregateCognition({item}, {11}, {rejected}), {rejected}));
    Check(report.at("invalidBindings") == 1 && report.at("runs").front().at("answerValuesPassed").is_null(),
        "Changed frozen source retained a serialized value judgment.");
    report = nlohmann::json::parse(CognitionReportJson(AggregateCognition({item}, {11}, {original, original}), {original, original}));
    Check(report.at("recordedRuns") == 1 && report.at("semanticPassed") == 1 && !report.at("errors").empty() &&
              report.at("runs").at(1).at("bindingVerified") == false && report.at("runs").at(1).at("semanticPassed").is_null(),
        "Duplicate evidence retained a second serialized approval.");
    rejected = original;
    rejected.caseId = "not-in-frozen-corpus";
    report = nlohmann::json::parse(CognitionReportJson(AggregateCognition({item}, {11}, {rejected}), {rejected}));
    Check(report.at("recordedRuns") == 0 && report.at("missingRuns") == 1 &&
              report.at("runs").front().at("finalAnswerDiagnostic") == "unjudged",
        "Unexpected evidence received a serialized judgment or filled a frozen slot.");
    const auto snapshot = AggregateCognition({item}, {11}, {original});
    report = nlohmann::json::parse(CognitionReportJson(snapshot, {rejected}));
    Check(report.at("runs").front().at("caseId") == item.id && report.at("runs").front().at("semanticPassed") == true,
        "Original run arguments replaced the report's admitted snapshot.");
    report = nlohmann::json::parse(CognitionReportJson(CognitionReport{}, {original}));
    Check(report.at("runs").empty(), "A report without admitted evidence serialized supplied raw approvals.");
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
    Check(valid.finalAnswerDiagnostic == StructuredAnswerDiagnostic::Match && valid.answerValuesPassed == true,
        "Valid evidence admission lost a comparable structured answer.");
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
    Check(report.structuredMatch == 0 && report.structuredUnjudged == 1, "Different build retained a structured diagnostic judgment.");
    auto reviewed = valid;
    reviewed.personalityPassed = true;
    reviewed.repairIntroducedFailure = true;
    reviewed.repairRescuedAnswer = true;
    auto serialized = nlohmann::json::parse(CognitionReportJson(AggregateBoundCognition({item}, {swapped}, {reviewed}), {reviewed}));
    const auto& rejected = serialized.at("runs").front();
    Check(rejected.at("bindingVerified") == false && rejected.at("finalAnswerDiagnostic") == "unjudged" &&
              rejected.at("rawAnswerDiagnostic") == "unjudged" && rejected.at("answerValuesPassed").is_null() &&
              rejected.at("rawAnswerValuesPassed").is_null(),
        "Serialized report trusted original run judgments after campaign admission failed.");
    Check(rejected.at("semanticPassed").is_null() && rejected.at("oraclePassed").is_null() && rejected.at("personalityPassed").is_null() &&
              rejected.at("reviewerId") == "" && rejected.at("mechanicalPassed") == false &&
              rejected.at("repairIntroducedFailure") == false && rejected.at("repairRescuedAnswer") == false,
        "Rejected campaign retained a serialized semantic, personality or repair approval.");
    Check(rejected.at("outputDigest") == reviewed.outputDigest && rejected.at("evidence").size() == 2,
        "Admission normalization removed rejected raw evidence identity from inspection.");
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
    const auto cleared = [](const CognitionVerdict& verdict)
    {
        return verdict.finalAnswerDiagnostic == StructuredAnswerDiagnostic::Unjudged &&
               verdict.rawAnswerDiagnostic == StructuredAnswerDiagnostic::Unjudged && !verdict.answerValuesPassed.has_value() &&
               !verdict.rawAnswerValuesPassed.has_value() && !verdict.repairIntroducedFailure && !verdict.repairRescuedAnswer;
    };
    Check(cleared(refused), "Cross-audience evidence retained a new diagnostic judgment.");
    result.digest = std::string(64, '7');
    const auto wrongOutput = revia::core::EvidenceBundle::Create({source, result}, task.stamp, task.scope, validation);
    refused = EvaluateBoundCognitionCase(item, output, campaign, *wrongOutput, task, task.stamp, task.scope, guard, oracles);
    Check(!refused.bindingVerified && !refused.semanticPassed, "Different output evidence entered the semantic numerator.");
    Check(cleared(refused), "Substituted output evidence retained a new diagnostic judgment.");
    std::stop_source stop;
    stop.request_stop();
    refused =
        EvaluateBoundCognitionCase(item, output, campaign, *wrongOutput, task, task.stamp, task.scope, guard, oracles, stop.get_token());
    Check(!refused.bindingVerified, "Cancelled task admitted campaign evidence.");
    Check(cleared(refused), "Cancelled evidence retained a new diagnostic judgment.");
    auto changed = Output(item, R"({"objects":11})");
    changed.turns.back().rawReply = item.expectedJson;
    refused = EvaluateBoundCognitionCase(item, changed, campaign, *bundle, task, task.stamp, task.scope, guard, oracles);
    Check(cleared(refused), "Failed output binding retained an apparent repair-induced failure.");
    auto unavailable = output;
    unavailable.unavailable = true;
    Check(cleared(EvaluateCognitionCase(item, unavailable, 11, oracles)), "Unavailable model output was assigned structured judgments.");
    auto substituted = output;
    substituted.turns.front().input = "foreign source";
    Check(cleared(EvaluateCognitionCase(item, substituted, 11, oracles)), "Foreign source was assigned structured judgments.");
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
            TestStructuredDiagnostics();
            TestDiagnosticAccounting();
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
