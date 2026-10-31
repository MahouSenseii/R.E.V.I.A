#include "Learning/liveDatasetExport.h"

#include "Actions/actionTypes.h"
#include "Audit/contentDigest.h"
#include "Core/atomicFile.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::learning
{
namespace
{
using nlohmann::json;

constexpr const char* LineageSchema = "revia-dataset-lineage-v1";

bool Refuse(std::string& error, std::string reason)
{
    error = std::move(reason);
    return false;
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool WriteReplacing(const std::filesystem::path& target, const std::string& bytes, std::string& error)
{
    auto partial = target;
    partial += ".partial";
    {
        std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
        stream << bytes;
        if (!stream.flush())
        {
            error = "A file could not be written.";
            return false;
        }
    }
    return core::ReplaceFileAtomically(target, partial, error);
}

// The folders OneDrive keeps in step with the cloud, as Windows names them.
std::vector<std::filesystem::path> SyncedRoots()
{
    std::vector<std::filesystem::path> roots;
#ifdef _WIN32
    for (const auto* name : {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"})
        if (const wchar_t* value = _wgetenv(name); value && *value)
            roots.emplace_back(value);
#else
    for (const auto* name : {"OneDrive", "OneDriveConsumer", "OneDriveCommercial"})
        if (const char* value = std::getenv(name); value && *value)
            roots.emplace_back(value);
#endif
    return roots;
}

bool Inside(const std::filesystem::path& path, const std::filesystem::path& root)
{
#ifdef _WIN32
    const auto value = path.lexically_normal().generic_wstring();
    auto prefix = root.lexically_normal().generic_wstring();
    while (prefix.size() > 1 && prefix.back() == L'/')
        prefix.pop_back();
    return value.size() >= prefix.size() &&
           CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(prefix.size()), TRUE) ==
               CSTR_EQUAL &&
           (value.size() == prefix.size() || value[prefix.size()] == L'/');
#else
    const auto relative = path.lexically_normal().lexically_relative(root.lexically_normal());
    return !relative.empty() && *relative.begin() != "..";
#endif
}

template <typename Text>
bool NetworkPath(const Text& native)
{
    const auto separator = [](const auto character) { return character == '/' || character == '\\'; };
    return native.size() >= 2 && separator(native[0]) && separator(native[1]);
}

struct Lineage
{
    std::string name;
    std::string salt;
    json exports = json::array();
};

std::filesystem::path ManifestCopy(const std::filesystem::path& lineageRoot, const std::string& name)
{
    return lineageRoot / "manifests" / (name + ".json");
}

bool ReadLineage(const std::filesystem::path& lineageRoot, const std::string& lineageName, Lineage& lineage, std::string& error)
{
    std::error_code ignored;
    const auto path = lineageRoot / "lineage.json";
    if (!std::filesystem::exists(path, ignored))
    {
        lineage = {lineageName, lineageName + "-salt-1", json::array()};
        return true;
    }
    try
    {
        const auto data = json::parse(ReadText(path));
        if (data.at("schema") != LineageSchema || data.at("name") != lineageName || !data.at("exports").is_array())
            throw std::runtime_error("lineage");
        lineage = {lineageName, data.at("salt").get<std::string>(), data.at("exports")};
        if (lineage.salt.empty())
            throw std::runtime_error("salt");
        return true;
    }
    catch (const std::exception&)
    {
        return Refuse(error, "The dataset lineage could not be read, so earlier partitions cannot be kept. Nothing was exported.");
    }
}

// Every partition an earlier export gave, as one manifest the exporter can keep them from.
// The first placement wins; the exporter never moved one, so there is nothing to reconcile.
bool PriorPartitions(const std::filesystem::path& lineageRoot, const Lineage& lineage, std::string& prior, std::string& error)
{
    prior.clear();
    if (lineage.exports.empty())
        return true;
    std::map<std::string, std::string> placed;
    json examples = json::array();
    try
    {
        for (const auto& earlier : lineage.exports)
        {
            const auto name = earlier.at("name").get<std::string>();
            const auto bytes = ReadText(ManifestCopy(lineageRoot, name));
            if (bytes.empty() || audit::ContentDigest(bytes) != earlier.at("manifestSha256").get<std::string>())
                return Refuse(error, "The kept manifest of " + name +
                                         " is missing or changed, so its partitions cannot be kept. Nothing was exported.");
            // Named, so the loop does not run over a member of a destroyed temporary.
            const auto manifest = json::parse(bytes);
            for (const auto& example : manifest.at("examples"))
                if (example.at("eligible").get<bool>() && placed.emplace(example.at("id").get<std::string>(), example.at("partition")).second)
                    examples.push_back({{"id", example.at("id")}, {"eligible", true}, {"partition", example.at("partition")}});
        }
    }
    catch (const std::exception&)
    {
        return Refuse(error, "An earlier export's kept manifest could not be read, so its partitions cannot be kept. Nothing was exported.");
    }
    prior = json{{"schema", "revia-dataset-v1"}, {"examples", std::move(examples)}}.dump();
    return true;
}
} // namespace

std::string PrivateDataFolderRefusal(const std::filesystem::path& folder)
{
    // Checked first: libstdc++ on Windows does not count a UNC path as absolute.
    if (NetworkPath(folder.native()))
        return "Private training data is not exported to a network folder; choose a local one.";
    if (folder.empty() || !folder.is_absolute())
        return "Choose the export folder by its full path.";
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error))
        return "That export folder does not exist.";
    for (auto part = folder.lexically_normal();; part = part.parent_path())
    {
        const auto status = std::filesystem::symlink_status(part, error);
        if (error)
            return "The export folder's path could not be checked.";
        if (std::filesystem::is_symlink(status))
            return "The export folder's path goes through a link; choose the folder it points to.";
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(part.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return "The export folder's path goes through a link or reparse point; choose a plain local folder.";
#endif
        if (!part.has_relative_path() || part.parent_path() == part)
            break;
    }
    for (const auto& root : SyncedRoots())
        if (Inside(folder, root))
            return "Private training data stays outside OneDrive, which would upload it. Choose a local folder such as D:\\ReviaTraining.";
    return {};
}

bool ExportLiveDataset(const LiveCaptureStore& store, const std::filesystem::path& lineageRoot, const std::string& lineageName,
    const std::filesystem::path& parent, LiveDatasetExport& outExport, std::string& outError)
{
    outExport = {};
    outError.clear();
    if (const auto refusal = PrivateDataFolderRefusal(parent); !refusal.empty())
        return Refuse(outError, refusal);
    Lineage lineage;
    std::string prior;
    if (!ReadLineage(lineageRoot, lineageName, lineage, outError) || !PriorPartitions(lineageRoot, lineage, prior, outError))
        return false;

    std::string candidateFile;
    std::size_t decided = 0;
    std::vector<DatasetCandidate> candidates;
    if (!store.CandidateFile(candidateFile, decided, outExport.waiting, outError) ||
        !ReadDatasetCandidates(candidateFile, candidates, outError))
        return false;
    if (candidates.empty())
        return Refuse(outError, outExport.waiting > 0 ? "No kept capture has a decision yet; save a correction first. Nothing was exported."
                                                      : "No exchange is kept for training. Nothing was exported.");
    std::set<std::string> withdrawn;
    std::error_code ignored;
    if (std::filesystem::exists(store.WithdrawnList(), ignored))
    {
        try
        {
            for (const auto& source : json::parse(ReadText(store.WithdrawnList())).get<std::vector<std::string>>())
                withdrawn.insert(source);
        }
        catch (const std::exception&)
        {
            return Refuse(outError, "The withdrawal list could not be read, so nothing was exported.");
        }
    }

    outExport.number = lineage.exports.size() + 1;
    outExport.name = lineage.name + "-" + std::to_string(outExport.number);
    DatasetExport exported;
    if (!BuildDatasetExport(candidates, withdrawn, {outExport.name, lineage.salt, LiveValidationPercent, LiveTestPercent}, prior,
            exported, outError))
        return false;
    for (const auto& decision : exported.decisions)
    {
        if (!decision.eligible)
            outExport.excluded.push_back(decision);
        else if (decision.partition == DatasetPartition::Train)
            ++outExport.train;
        else if (decision.partition == DatasetPartition::Validation)
            ++outExport.validation;
        else
            ++outExport.test;
    }
    if (outExport.train == 0)
    {
        const std::size_t heldOut = outExport.validation + outExport.test;
        std::string reasons;
        for (const auto& decision : outExport.excluded)
            reasons += (reasons.empty() ? " Left out: " : "; ") + decision.exampleId + " (" + decision.reason + ")";
        outExport = {};
        return Refuse(outError, (heldOut == 0 ? std::string("No accepted correction is eligible to export.")
                                              : "Every eligible example falls in a held-out partition, so there is nothing to train on "
                                                "yet; save more corrections first.") +
                                    reasons + " Nothing was exported.");
    }
    outExport.directory = parent / outExport.name;
    if (std::filesystem::exists(outExport.directory, ignored))
        return Refuse(outError, actions::PathToUtf8(outExport.directory) + " already exists; an export is never replaced.");

    // The lineage is recorded before the export is written: should writing fail, the next
    // export still keeps these partitions rather than reusing their examples elsewhere.
    const auto& manifest = exported.files.at("manifest.json");
    outExport.manifestSha256 = audit::ContentDigest(manifest);
    std::filesystem::create_directories(lineageRoot / "manifests", ignored);
    auto record = json{{"number", outExport.number}, {"name", outExport.name}, {"directory", actions::PathToUtf8(outExport.directory)},
        {"manifestSha256", outExport.manifestSha256}, {"train", outExport.train}, {"validation", outExport.validation},
        {"test", outExport.test}};
    lineage.exports.push_back(std::move(record));
    const json saved = {{"schema", LineageSchema}, {"name", lineage.name}, {"salt", lineage.salt}, {"exports", lineage.exports}};
    if (!WriteReplacing(ManifestCopy(lineageRoot, outExport.name), manifest, outError) ||
        !WriteReplacing(lineageRoot / "lineage.json", saved.dump(2) + '\n', outError))
        return Refuse(outError, "The dataset lineage could not be recorded, so nothing was exported: " + outError);
    if (!WriteDatasetExport(exported, outExport.directory, outError))
        return false;
    // A training job names this list; it exists from the first export even before anything is withdrawn.
    outExport.withdrawnList = store.WithdrawnList();
    if (!std::filesystem::exists(outExport.withdrawnList, ignored))
    {
        std::string error;
        if (!WriteReplacing(outExport.withdrawnList, "[]\n", error))
            outExport.withdrawnList.clear();
    }
    return true;
}

std::string DescribeLiveDatasetExport(const LiveDatasetExport& value)
{
    std::string text = "Exported " + value.name + " to " + actions::PathToUtf8(value.directory) + ": " +
                       std::to_string(value.train + value.validation + value.test) + " examples (train " + std::to_string(value.train) +
                       ", validation " + std::to_string(value.validation) + ", test " + std::to_string(value.test) + ").";
    if (!value.excluded.empty())
    {
        text += " " + std::to_string(value.excluded.size()) + " left out:";
        for (std::size_t index = 0; index < value.excluded.size() && index < 3; ++index)
            text += (index == 0 ? " " : "; ") + value.excluded[index].exampleId + " (" + value.excluded[index].reason + ")";
        text += value.excluded.size() > 3 ? "; and more in the manifest." : ".";
    }
    if (value.waiting > 0)
        text += " " + std::to_string(value.waiting) + (value.waiting == 1 ? " kept capture still needs" : " kept captures still need") +
                " a decision.";
    if (value.test < LiveMinimumTestCases)
        text += " The evaluation plan needs at least " + std::to_string(LiveMinimumTestCases) + " held-out test cases; this export has " +
                std::to_string(value.test) + ".";
    if (!value.withdrawnList.empty())
        text += " A training job names " + actions::PathToUtf8(value.withdrawnList) + " as its withdrawnSourcesPath.";
    return text;
}

} // namespace revia::learning
