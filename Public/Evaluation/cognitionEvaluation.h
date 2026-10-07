#pragma once

#include "Evaluation/conversationEvaluation.h"
#include "Evaluation/campaignManifest.h"
#include "Core/evidenceBundle.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>

namespace revia::evaluation
{
struct CognitionCase
{
    std::string id;
    std::string family;
    std::string oracleId;
    EvaluationCase conversation;
    std::string expectedJson;
    std::string sourceDigest;
    std::vector<std::string> entityIds;
    std::vector<std::string> requiredClaims;
    std::vector<std::string> forbiddenClaims;
    std::string uncertaintyDisposition;
};

enum class StructuredAnswerDiagnostic
{
    Unjudged,
    InvalidJson,
    ShapeMismatch,
    ValueMismatch,
    Match
};

[[nodiscard]] std::string ToString(StructuredAnswerDiagnostic diagnostic);

struct CognitionVerdict
{
    std::string caseId;
    std::uint64_t seed = 0;
    bool available = false;
    bool mechanicalPassed = false;
    bool bindingVerified = false;
    std::optional<bool> semanticPassed;
    std::optional<bool> oraclePassed;
    std::optional<bool> personalityPassed;
    std::string reviewerId;
    std::string outputDigest;
    std::string sourceDigest;
    std::string diagnostic;
    std::string criterionDigest;
    std::string campaignDigest;
    std::vector<core::EvidenceRef> evidence;
    StructuredAnswerDiagnostic finalAnswerDiagnostic = StructuredAnswerDiagnostic::Unjudged;
    StructuredAnswerDiagnostic rawAnswerDiagnostic = StructuredAnswerDiagnostic::Unjudged;
    // Value judgments require complete JSON with the expected structure and types.
    std::optional<bool> answerValuesPassed;
    std::optional<bool> rawAnswerValuesPassed;
    bool repairIntroducedFailure = false;
    bool repairRescuedAnswer = false;
};

struct CognitionReview
{
    std::string caseId;
    std::uint64_t seed = 0;
    std::string reviewerId;
    std::string outputDigest;
    std::string sourceDigest;
    std::optional<bool> semanticPassed;
    std::optional<bool> personalityPassed;
    std::string criterionDigest;
    std::string campaignDigest;
};

class OracleRegistry
{
  public:
    using Checker = std::function<std::optional<bool>(const CognitionCase&, const CaseOutcome&)>;
    OracleRegistry();
    bool Register(std::string id, Checker checker);
    [[nodiscard]] std::optional<bool> Judge(const CognitionCase& item, const CaseOutcome& output) const;
    [[nodiscard]] bool Contains(const std::string& id) const;

  private:
    std::map<std::string, Checker> checkers;
};

struct CognitionReport
{
    std::size_t uniqueCases = 0;
    std::size_t expectedRuns = 0;
    std::size_t recordedRuns = 0;
    std::size_t missingRuns = 0;
    std::size_t unavailable = 0;
    std::size_t invalidBindings = 0;
    std::size_t mechanicalPassed = 0;
    std::size_t semanticPassed = 0;
    std::size_t semanticFailed = 0;
    std::size_t semanticUnjudged = 0;
    std::size_t personalityPassed = 0;
    std::size_t personalityFailed = 0;
    std::size_t personalityUnjudged = 0;
    std::size_t invalidJson = 0;
    std::size_t shapeMismatch = 0;
    std::size_t valueMismatch = 0;
    std::size_t structuredMatch = 0;
    std::size_t structuredUnjudged = 0;
    std::size_t repairIntroducedFailures = 0;
    std::size_t repairRescuedAnswers = 0;
    bool liveQualified = false;
    std::vector<std::string> errors;
    std::vector<CognitionVerdict> normalizedRuns;
};

[[nodiscard]] CognitionVerdict EvaluateCognitionCase(
    const CognitionCase& item, const CaseOutcome& output, std::uint64_t seed, const OracleRegistry& oracles);
[[nodiscard]] CognitionVerdict EvaluateBoundCognitionCase(const CognitionCase& item, const CaseOutcome& output,
    const CampaignManifest& campaign, const core::EvidenceBundle& evidence, const core::TaskContract& task,
    const runtime::RuntimeStamp& current, const memory::MemoryScope& scope, const core::CurrentStampGuard& guard,
    const OracleRegistry& oracles, std::stop_token cancellation = {});
[[nodiscard]] bool ApplyCognitionReview(CognitionVerdict& verdict, const CognitionReview& review, std::string& error);
[[nodiscard]] CognitionReport AggregateCognition(
    const std::vector<CognitionCase>& cases, const std::vector<std::uint64_t>& seeds, const std::vector<CognitionVerdict>& runs);
[[nodiscard]] CognitionReport AggregateBoundCognition(
    const std::vector<CognitionCase>& cases, const std::vector<CampaignManifest>& campaigns, const std::vector<CognitionVerdict>& runs);
[[nodiscard]] bool LoadCognitionCorpus(
    const std::filesystem::path& path, std::vector<CognitionCase>& cases, std::vector<std::uint64_t>& seeds, std::string& error);
[[nodiscard]] bool ValidateCognitionPartitions(const std::vector<CognitionCase>& development, const std::vector<CognitionCase>& calibration,
    const std::vector<CognitionCase>& heldout, std::string& error);
[[nodiscard]] std::string CognitionSourceDigest(const EvaluationCase& item);
[[nodiscard]] std::string CognitionSourceBytes(const EvaluationCase& item);
[[nodiscard]] std::string CognitionOutputBytes(const CaseOutcome& output);
[[nodiscard]] std::string CognitionCriterionDigest(const CognitionCase& item);
// Serialization uses the report's normalized admission snapshot; the second argument preserves existing callers.
[[nodiscard]] std::string CognitionReportJson(const CognitionReport& report, const std::vector<CognitionVerdict>& runs);
}
