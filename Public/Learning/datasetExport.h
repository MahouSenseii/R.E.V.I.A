#pragma once

#include "Learning/learningRecordStore.h"
#include "Learning/qualityFeedback.h"
#include "Runtime/runtimeStamp.h"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace revia::learning
{

// Supervised examples for optional offline training. This changes no memory, skill or
// model: it turns reviewed, provenance-bound candidates into deterministic files that a
// separate worker may read later.

struct DatasetMessage
{
    // "system", "user", "assistant" or "tool".
    std::string role;
    std::string content;
};

enum class DatasetTargetOrigin
{
    // An owner-written or owner-accepted replacement for a judged failure.
    ReviewedCorrection,
    // The model's own output, accepted by a recorded criterion.
    VerifiedOutput
};

// One example as the runtime captures it. A failed answer is never the target: it is
// kept only as the rejected side of its correction.
struct DatasetCandidate
{
    std::string exampleId;
    runtime::RuntimeStamp origin;
    // Every attachment, memory, document or derived extract the input used. Withdrawing
    // any of them makes the example ineligible.
    std::vector<std::string> sourceIds;
    // The exact model input, ending with the turn being answered.
    std::vector<DatasetMessage> prompt;
    // The tool definitions the request offered, as the exact JSON array sent, or empty when
    // none were. The chat template renders them into the prompt, so the example needs them.
    std::string tools;
    std::string response;
    DatasetTargetOrigin targetOrigin = DatasetTargetOrigin::ReviewedCorrection;
    std::string rejectedResponse;
    // The quality receipt: criterion, judged output digest and captured profile and
    // configuration fingerprints.
    QualityFeedback judgment;
    // The owner's decision on this example itself, separate from the judgment.
    LearningDecision review = LearningDecision::Pending;
    std::string modelId;
    std::string templateId;
    std::string transformationVersion;
};

struct DatasetExportOptions
{
    std::string datasetName;
    // Fixed for a dataset's lineage; a different salt reshuffles every new group.
    std::string salt;
    unsigned validationPercent = 10;
    unsigned testPercent = 10;
};

enum class DatasetPartition
{
    Train,
    Validation,
    Test
};

[[nodiscard]] std::string ToString(DatasetPartition value);

struct DatasetDecision
{
    std::string exampleId;
    bool eligible = false;
    // Why an example was excluded, or "eligible".
    std::string reason;
    DatasetPartition partition = DatasetPartition::Train;
    std::string groupId;
};

struct DatasetExport
{
    // File name to exact bytes, including manifest.json.
    std::map<std::string, std::string> files;
    std::vector<DatasetDecision> decisions;
};

// Groups related sessions, tasks, sources and identical prompts before splitting, so no
// group straddles partitions. An example a previous export placed keeps that partition,
// and one whose group would move it is excluded instead. The same inputs give the same
// bytes in any order.
bool BuildDatasetExport(const std::vector<DatasetCandidate>& candidates, const std::set<std::string>& withdrawnSources,
    const DatasetExportOptions& options, const std::string& priorManifest, DatasetExport& outExport, std::string& outError);

// For the moment before a worker reads an export: the exported examples that a source
// withdrawn since then makes ineligible. Any result means the export must not be used.
bool StaleDatasetExamples(const std::string& manifestBytes, const std::set<std::string>& withdrawnSources,
    std::vector<std::string>& outExampleIds, std::string& outError);

// Writes a new directory and never replaces one, so an export stays immutable.
bool WriteDatasetExport(const DatasetExport& value, const std::filesystem::path& directory, std::string& outError);

// Reads the candidate file a capture tool writes (schema "revia-dataset-candidates-v1").
// Strict: an unknown enumeration value, a missing field or non-text message content refuses
// the whole file. Eligibility stays with BuildDatasetExport.
bool ReadDatasetCandidates(const std::string& json, std::vector<DatasetCandidate>& outCandidates, std::string& outError);

} // namespace revia::learning
