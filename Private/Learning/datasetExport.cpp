#include "Learning/datasetExport.h"

#include "Audit/contentDigest.h"
#include "Memory/sensitiveContent.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <system_error>

namespace revia::learning
{
namespace
{
using nlohmann::json;

constexpr const char* Schema = "revia-dataset-v1";
constexpr std::size_t MaximumCandidates = 100000;
constexpr std::size_t MaximumMessages = 256;
constexpr std::size_t MaximumText = 64 * 1024;
constexpr std::size_t MaximumSources = 64;

const char* PartitionFile(const DatasetPartition value)
{
    switch (value)
    {
    case DatasetPartition::Train:
        return "train.jsonl";
    case DatasetPartition::Validation:
        return "validation.jsonl";
    case DatasetPartition::Test:
        return "test.jsonl";
    }
    return "train.jsonl";
}

std::optional<DatasetPartition> ParsePartition(const std::string& value)
{
    for (const auto partition : {DatasetPartition::Train, DatasetPartition::Validation, DatasetPartition::Test})
        if (ToString(partition) == value)
            return partition;
    return std::nullopt;
}

bool Bounded(const std::string& value, const std::size_t maximum)
{
    return !value.empty() && value.size() <= maximum && value.find('\0') == std::string::npos;
}

json MessagesJson(const std::vector<DatasetMessage>& messages)
{
    json value = json::array();
    for (const auto& message : messages)
        value.push_back({{"role", message.role}, {"content", message.content}});
    return value;
}

std::string PromptDigest(const DatasetCandidate& value)
{
    return audit::ContentDigest(MessagesJson(value.prompt).dump());
}

std::string ContentKey(const DatasetCandidate& value)
{
    return audit::ContentDigest(json{{"prompt", MessagesJson(value.prompt)}, {"response", value.response}}.dump());
}

std::vector<std::string> AllSources(const DatasetCandidate& value)
{
    // The judged turn and its source are withdrawn with the conversation they belong to.
    std::vector<std::string> sources = value.sourceIds;
    sources.push_back(value.judgment.sourceId);
    sources.push_back(value.judgment.targetId);
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    return sources;
}

// Why the example cannot be a training target, or an empty string.
std::string Ineligibility(const DatasetCandidate& value, const std::set<std::string>& withdrawn)
{
    std::string error;
    if (!ValidateQualityFeedback(value.judgment, error))
        return "its quality receipt is incomplete";
    if (value.judgment.origin.companionId != value.origin.companionId || value.judgment.origin.sessionId != value.origin.sessionId)
        return "its quality receipt belongs to another session";
    if (!value.judgment.dependencies.contains("loaded-profile-sha256") ||
        !value.judgment.dependencies.contains("effective-llm-config-sha256") || !Bounded(value.modelId, 256) ||
        !Bounded(value.templateId, 256) || !Bounded(value.transformationVersion, 128))
        return "its model, template or configuration provenance is missing";
    for (const auto& source : AllSources(value))
        if (withdrawn.contains(source))
            return "a source it used was withdrawn";
    if (value.review != LearningDecision::Accept)
        return "its review has not accepted it";
    if (value.prompt.empty() || value.prompt.size() > MaximumMessages ||
        (value.prompt.back().role != "user" && value.prompt.back().role != "tool") ||
        std::any_of(value.prompt.begin(), value.prompt.end(),
            [](const DatasetMessage& message)
            {
                return (message.role != "system" && message.role != "user" && message.role != "assistant" && message.role != "tool") ||
                    !Bounded(message.content, MaximumText);
            }) ||
        !Bounded(value.response, MaximumText) || value.rejectedResponse.size() > MaximumText || value.sourceIds.size() > MaximumSources)
        return "its messages are empty, unbounded or do not end with the turn being answered";
    if (value.targetOrigin == DatasetTargetOrigin::ReviewedCorrection)
    {
        // The rejected side must be exactly the output the owner judged a failure.
        if (value.judgment.source != QualityEvidenceSource::OwnerJudgment || value.judgment.criterionSatisfied ||
            audit::ContentDigest(value.rejectedResponse) != value.judgment.targetDigest)
            return "its correction is not bound to the exact judged failure";
        if (value.response == value.rejectedResponse)
            return "its correction repeats the failed answer";
    }
    else if (!value.judgment.criterionSatisfied || audit::ContentDigest(value.response) != value.judgment.targetDigest ||
             !value.rejectedResponse.empty())
        return "its output was not the one the recorded criterion accepted";
    std::string text;
    for (const auto& message : value.prompt)
        text += message.content + '\n';
    if (const auto finding = memory::DetectSensitiveContent(text + value.response + '\n' + value.rejectedResponse))
        return "it contains " + finding.description;
    return {};
}

class Groups
{
public:
    explicit Groups(const std::size_t size) : parent(size) { std::iota(parent.begin(), parent.end(), std::size_t{0}); }
    std::size_t Find(std::size_t index)
    {
        while (parent[index] != index)
            index = parent[index] = parent[parent[index]];
        return index;
    }
    void Join(const std::size_t first, const std::size_t second)
    {
        const auto a = Find(first);
        const auto b = Find(second);
        // The smaller index stays the root, so the result does not depend on join order.
        parent[std::max(a, b)] = std::min(a, b);
    }

private:
    std::vector<std::size_t> parent;
};

DatasetPartition HashedPartition(const DatasetExportOptions& options, const std::string& groupId)
{
    const auto digest = audit::ContentDigest(options.salt + '\n' + groupId);
    const auto bucket = std::stoul(digest.substr(0, 8), nullptr, 16) % 100;
    if (bucket < options.testPercent)
        return DatasetPartition::Test;
    if (bucket < options.testPercent + options.validationPercent)
        return DatasetPartition::Validation;
    return DatasetPartition::Train;
}

bool PriorPartitions(const std::string& manifest, std::map<std::string, DatasetPartition>& output, std::string& error)
{
    if (manifest.empty())
        return true;
    try
    {
        const auto data = json::parse(manifest);
        if (data.at("schema").get<std::string>() != Schema)
            throw std::runtime_error("schema");
        for (const auto& example : data.at("examples"))
            if (example.at("eligible").get<bool>())
            {
                const auto partition = ParsePartition(example.at("partition").get<std::string>());
                if (!partition)
                    throw std::runtime_error("partition");
                output[example.at("id").get<std::string>()] = *partition;
            }
        return true;
    }
    catch (const std::exception&)
    {
        error = "The previous dataset manifest could not be read, so earlier partitions cannot be kept.";
        return false;
    }
}

json LineageJson(const DatasetCandidate& value)
{
    return {{"companion", value.origin.companionId}, {"session", value.origin.sessionId}, {"task", value.origin.taskId},
        {"sources", AllSources(value)}, {"judgment", value.judgment.judgmentId}, {"criterion", value.judgment.criterion},
        {"evidenceSource", value.judgment.source == QualityEvidenceSource::OwnerJudgment ? "owner judgment" : "deterministic acceptance"},
        {"targetDigest", value.judgment.targetDigest}, {"dependencies", value.judgment.dependencies}, {"model", value.modelId},
        {"template", value.templateId}, {"transformation", value.transformationVersion},
        {"origin", value.targetOrigin == DatasetTargetOrigin::ReviewedCorrection ? "reviewed correction" : "verified output"}};
}
}

std::string ToString(const DatasetPartition value)
{
    switch (value)
    {
    case DatasetPartition::Train:
        return "train";
    case DatasetPartition::Validation:
        return "validation";
    case DatasetPartition::Test:
        return "test";
    }
    return "train";
}

bool BuildDatasetExport(const std::vector<DatasetCandidate>& input, const std::set<std::string>& withdrawnSources,
    const DatasetExportOptions& options, const std::string& priorManifest, DatasetExport& outExport, std::string& outError)
{
    outExport = {};
    outError.clear();
    if (!Bounded(options.datasetName, 128) || !Bounded(options.salt, 128) || options.validationPercent > 50 || options.testPercent > 50 ||
        options.validationPercent + options.testPercent >= 100 || input.size() > MaximumCandidates)
    {
        outError = "A dataset export needs a bounded name and salt, partitions under 100% and at most 100,000 candidates.";
        return false;
    }
    std::map<std::string, DatasetPartition> prior;
    if (!PriorPartitions(priorManifest, prior, outError))
        return false;

    std::vector<DatasetCandidate> candidates = input;
    std::sort(candidates.begin(), candidates.end(),
        [](const DatasetCandidate& left, const DatasetCandidate& right) { return left.exampleId < right.exampleId; });
    for (std::size_t index = 0; index < candidates.size(); ++index)
        if (!Bounded(candidates[index].exampleId, 128) ||
            (index > 0 && candidates[index].exampleId == candidates[index - 1].exampleId))
        {
            outError = "Every dataset candidate needs its own bounded example ID.";
            return false;
        }

    std::vector<DatasetDecision> decisions(candidates.size());
    std::map<std::string, std::size_t> firstWithContent;
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        auto& decision = decisions[index];
        decision.exampleId = candidates[index].exampleId;
        decision.reason = Ineligibility(candidates[index], withdrawnSources);
        if (decision.reason.empty())
        {
            const auto [first, inserted] = firstWithContent.emplace(ContentKey(candidates[index]), index);
            if (!inserted)
                decision.reason = "it duplicates " + candidates[first->second].exampleId;
        }
        decision.eligible = decision.reason.empty();
        if (decision.eligible)
            decision.reason = "eligible";
    }

    // Related examples share one group: the same session, task, source or prompt.
    Groups groups(candidates.size());
    std::map<std::string, std::size_t> owners;
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        const auto& value = candidates[index];
        std::vector<std::string> keys = {"session:" + value.origin.companionId + '/' + value.origin.sessionId,
            "prompt:" + PromptDigest(value)};
        if (!value.origin.taskId.empty())
            keys.push_back("task:" + value.origin.companionId + '/' + value.origin.taskId);
        for (const auto& source : AllSources(value))
            keys.push_back("source:" + source);
        for (const auto& key : keys)
            if (const auto [owner, inserted] = owners.emplace(key, index); !inserted)
                groups.Join(owner->second, index);
    }

    // A group keeps any partition an earlier export gave its members. If its members were
    // placed apart before, the group follows the most protected one and the others are
    // excluded, so no example ever changes partition.
    std::map<std::size_t, DatasetPartition> groupPartition;
    for (std::size_t index = 0; index < candidates.size(); ++index)
        if (const auto earlier = prior.find(candidates[index].exampleId); earlier != prior.end())
        {
            auto [assigned, inserted] = groupPartition.emplace(groups.Find(index), earlier->second);
            if (!inserted && earlier->second > assigned->second)
                assigned->second = earlier->second;
        }
    std::map<DatasetPartition, json> lines;
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        auto& decision = decisions[index];
        const auto root = groups.Find(index);
        decision.groupId = audit::ContentDigest(candidates[root].exampleId).substr(0, 16);
        const auto assigned = groupPartition.find(root);
        decision.partition = assigned != groupPartition.end() ? assigned->second : HashedPartition(options, decision.groupId);
        if (const auto earlier = prior.find(decision.exampleId);
            decision.eligible && earlier != prior.end() && earlier->second != decision.partition)
        {
            decision.eligible = false;
            decision.reason = "its group now spans partitions an earlier export kept apart";
        }
        if (!decision.eligible)
            continue;
        const auto& value = candidates[index];
        json record = {{"id", value.exampleId}, {"lineage", LineageJson(value)}};
        record["messages"] = MessagesJson(value.prompt);
        record["messages"].push_back({{"role", "assistant"}, {"content", value.response}});
        if (!value.rejectedResponse.empty())
            record["rejected"] = value.rejectedResponse;
        lines[decision.partition].push_back(std::move(record));
    }

    json manifest = {{"schema", Schema}, {"name", options.datasetName}, {"salt", options.salt},
        {"validationPercent", options.validationPercent}, {"testPercent", options.testPercent}};
    std::string withdrawnList;
    for (const auto& source : withdrawnSources)
        withdrawnList += source + '\n';
    manifest["withdrawnSourcesDigest"] = audit::ContentDigest(withdrawnList);
    json files = json::object();
    for (const auto partition : {DatasetPartition::Train, DatasetPartition::Validation, DatasetPartition::Test})
    {
        std::string bytes;
        const auto found = lines.find(partition);
        const std::size_t count = found == lines.end() ? 0 : found->second.size();
        if (found != lines.end())
            for (const auto& line : found->second)
                bytes += line.dump() + '\n';
        files[PartitionFile(partition)] = {{"sha256", audit::ContentDigest(bytes)}, {"examples", count}};
        outExport.files[PartitionFile(partition)] = std::move(bytes);
    }
    manifest["files"] = std::move(files);
    json examples = json::array();
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        const auto& decision = decisions[index];
        json example = {{"id", decision.exampleId}, {"eligible", decision.eligible}, {"reason", decision.reason},
            {"sources", AllSources(candidates[index])}, {"contentDigest", ContentKey(candidates[index])}};
        if (decision.eligible)
        {
            example["partition"] = ToString(decision.partition);
            example["group"] = decision.groupId;
        }
        examples.push_back(std::move(example));
    }
    manifest["examples"] = std::move(examples);
    outExport.files["manifest.json"] = manifest.dump(2) + '\n';
    outExport.decisions = std::move(decisions);
    return true;
}

bool StaleDatasetExamples(const std::string& manifestBytes, const std::set<std::string>& withdrawnSources,
    std::vector<std::string>& outExampleIds, std::string& outError)
{
    outExampleIds.clear();
    outError.clear();
    try
    {
        const auto data = json::parse(manifestBytes);
        if (data.at("schema").get<std::string>() != Schema)
            throw std::runtime_error("schema");
        for (const auto& example : data.at("examples"))
        {
            if (!example.at("eligible").get<bool>())
                continue;
            const auto sources = example.at("sources").get<std::vector<std::string>>();
            if (std::any_of(sources.begin(), sources.end(), [&](const std::string& source) { return withdrawnSources.contains(source); }))
                outExampleIds.push_back(example.at("id").get<std::string>());
        }
        return true;
    }
    catch (const std::exception&)
    {
        outError = "The dataset manifest could not be read, so its eligibility cannot be rechecked.";
        return false;
    }
}

bool WriteDatasetExport(const DatasetExport& value, const std::filesystem::path& directory, std::string& outError)
{
    outError.clear();
    std::error_code error;
    const auto staging = directory.parent_path() / (directory.filename().string() + ".partial");
    if (std::filesystem::exists(directory, error) || std::filesystem::exists(staging, error))
    {
        outError = "A dataset export never replaces an existing directory or an unfinished one beside it.";
        return false;
    }
    const bool plainNames = std::all_of(value.files.begin(), value.files.end(),
        [](const auto& file) { return Bounded(file.first, 64) && file.first.find_first_of("/\\:") == std::string::npos && file.first[0] != '.'; });
    if (!plainNames || !value.files.contains("manifest.json") || !std::filesystem::create_directories(staging, error))
    {
        outError = "The dataset export has no manifest or a file name outside its directory, or staging failed.";
        return false;
    }
    for (const auto& [name, bytes] : value.files)
    {
        {
            std::ofstream stream(staging / name, std::ios::binary | std::ios::trunc);
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        std::ifstream check(staging / name, std::ios::binary);
        const std::string written{std::istreambuf_iterator<char>(check), std::istreambuf_iterator<char>()};
        if (audit::ContentDigest(written) != audit::ContentDigest(bytes))
        {
            std::filesystem::remove_all(staging, error);
            outError = "A dataset file did not read back as written: " + name;
            return false;
        }
    }
    std::filesystem::rename(staging, directory, error);
    if (error)
    {
        outError = "The finished dataset export could not be moved into place: " + error.message();
        std::filesystem::remove_all(staging, error);
        return false;
    }
    return true;
}

} // namespace revia::learning
