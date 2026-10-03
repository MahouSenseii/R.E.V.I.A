#include "Skills/skillPackage.h"

#include "Audit/contentDigest.h"
#include "Memory/sensitiveContent.h"
#include "packageStorage.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace revia::skills
{
namespace
{
using nlohmann::json;

bool Identifier(const std::string& value)
{
    return !value.empty() && value.size() <= 64 && value.front() != '.' &&
           std::all_of(value.begin(), value.end(), [](const unsigned char c) { return std::islower(c) || std::isdigit(c) || c == '-'; });
}

bool Version(const std::string& value)
{
    static const std::regex expression(R"([0-9]{1,4}\.[0-9]{1,4}\.[0-9]{1,4})");
    return std::regex_match(value, expression);
}

std::string Key(const SkillPackageReference& reference)
{
    return reference.id + "@" + reference.version;
}

void ExactKeys(const json& value, const std::set<std::string>& keys)
{
    if (!value.is_object() || value.size() != keys.size())
        throw std::runtime_error("Package schema is not exact.");
    for (const auto& item : value.items())
        if (!keys.contains(item.key()))
            throw std::runtime_error("Unknown package field.");
}

json CasesJson(const std::vector<SkillCase>& values)
{
    json cases = json::array();
    for (const auto& value : values)
        cases.push_back({{"id", value.id}, {"entries", value.entries}, {"expectedSummary", value.expectedSummary}});
    return cases;
}

std::vector<SkillCase> DecodeCases(const json& values)
{
    if (!values.is_array() || values.empty() || values.size() > 32)
        throw std::runtime_error("Package cases are not bounded.");
    std::vector<SkillCase> result;
    std::set<std::string> ids;
    for (const auto& value : values)
    {
        ExactKeys(value, {"id", "entries", "expectedSummary"});
        SkillCase entry{value.at("id").get<std::string>(), value.at("entries").get<std::vector<std::string>>(),
            value.at("expectedSummary").get<std::string>()};
        if (!Identifier(entry.id) || !ids.insert(entry.id).second || entry.entries.size() > 64 || entry.expectedSummary.empty() ||
            entry.expectedSummary.size() > 1024 ||
            std::any_of(entry.entries.begin(), entry.entries.end(),
                [](const auto& text) { return text.empty() || text.size() > 256 || text.find('\n') != std::string::npos; }))
            throw std::runtime_error("Package case is invalid.");
        result.push_back(std::move(entry));
    }
    return result;
}

json ManifestJson(const SkillManifest& value)
{
    json required = json::array(), optional = json::array();
    for (const auto operation : value.requiredCapabilities)
        required.push_back(actions::ToString(operation));
    for (const auto operation : value.optionalCapabilities)
        optional.push_back(actions::ToString(operation));
    return {{"schemaVersion", value.schemaVersion}, {"id", value.id}, {"version", value.version}, {"supportedTasks", value.supportedTasks},
        {"inputSchema", json::parse(value.inputSchema)}, {"outputSchema", json::parse(value.outputSchema)},
        {"requiredCapabilities", required}, {"optionalCapabilities", optional}, {"sideEffects", value.sideEffects},
        {"supportedToolVersions", value.supportedToolVersions}, {"verificationEntry", value.verificationEntry},
        {"outputFormat", value.outputFormat}};
}

SkillManifest DecodeManifest(const json& value)
{
    ExactKeys(value, {"schemaVersion", "id", "version", "supportedTasks", "inputSchema", "outputSchema", "requiredCapabilities",
                         "optionalCapabilities", "sideEffects", "supportedToolVersions", "verificationEntry", "outputFormat"});
    SkillManifest result;
    if ((!value.at("schemaVersion").is_number_unsigned() && !value.at("schemaVersion").is_number_integer()) ||
        value.at("schemaVersion") != 1)
        throw std::runtime_error("Invalid package schema version.");
    result.schemaVersion = value.at("schemaVersion").get<std::uint32_t>();
    result.id = value.at("id").get<std::string>();
    result.version = value.at("version").get<std::string>();
    result.supportedTasks = value.at("supportedTasks").get<std::vector<std::string>>();
    if (!value.at("inputSchema").is_object() || !value.at("outputSchema").is_object())
        throw std::runtime_error("Package schemas must be objects.");
    result.inputSchema = value.at("inputSchema").dump();
    result.outputSchema = value.at("outputSchema").dump();
    std::set<actions::ActionType> seen;
    const auto capabilities = [&](const json& list, std::vector<actions::ActionType>& destination)
    {
        if (!list.is_array() || list.size() > 16)
            throw std::runtime_error("Invalid capability requirements.");
        for (const auto& item : list)
        {
            const std::string name = item.get<std::string>();
            const auto operation = actions::ActionTypeFromString(name);
            if (operation == actions::ActionType::Unknown || actions::ToString(operation) != name || !seen.insert(operation).second)
                throw std::runtime_error("Unknown or duplicate capability requirement.");
            destination.push_back(operation);
        }
    };
    capabilities(value.at("requiredCapabilities"), result.requiredCapabilities);
    capabilities(value.at("optionalCapabilities"), result.optionalCapabilities);
    result.sideEffects = value.at("sideEffects").get<std::vector<std::string>>();
    result.supportedToolVersions = value.at("supportedToolVersions").get<std::map<std::string, std::string>>();
    result.verificationEntry = value.at("verificationEntry").get<std::string>();
    result.outputFormat = value.at("outputFormat").get<std::string>();
    if (result.schemaVersion != 1 || result.id != "workspace-inventory" || !Version(result.version) ||
        result.supportedTasks != std::vector<std::string>{"workspace-inventory"} || result.requiredCapabilities.empty() ||
        std::find(result.requiredCapabilities.begin(), result.requiredCapabilities.end(), actions::ActionType::ListDirectory) ==
            result.requiredCapabilities.end() ||
        result.sideEffects != std::vector<std::string>{"none"} || result.supportedToolVersions.empty() ||
        result.supportedToolVersions.size() > 8 || result.inputSchema.size() > 4096 || result.outputSchema.size() > 4096 ||
        result.verificationEntry != "cases.json" || (result.outputFormat != "counts" && result.outputFormat != "typed-counts"))
        throw std::runtime_error("Unsupported bounded package contract.");
    for (const auto& [tool, version] : result.supportedToolVersions)
        if (!Identifier(tool) || version.empty() || version.size() > 64)
            throw std::runtime_error("Unbounded tool requirement.");
    return result;
}

std::string PackageDigest(const SkillPackage& value)
{
    return audit::ContentDigest(
        json{{"manifest", ManifestJson(value.manifest)}, {"procedure", value.procedure}, {"cases", CasesJson(value.cases)}}.dump());
}

bool Neutral(const SkillPackage& value)
{
    std::string content = ManifestJson(value.manifest).dump() + "\n" + value.procedure + "\n" + CasesJson(value.cases).dump();
    if (memory::ContainsSensitiveContent(content))
        return false;
    std::transform(
        content.begin(), content.end(), content.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::regex privatePath(R"(([a-z]:[/\\])|(/users/)|(/home/)|(\\\\))");
    if (std::regex_search(content, privatePath))
        return false;
    for (const auto* marker :
        {"private:", "companion-private", "alias:", "relationship:", "remembered conversation", "i remember", "i am revia"})
        if (content.find(marker) != std::string::npos)
            return false;
    return true;
}

void ValidatePackage(const SkillPackage& value)
{
    const auto manifest = DecodeManifest(ManifestJson(value.manifest));
    (void)DecodeCases(CasesJson(value.cases));
    if (value.procedure.empty() || value.procedure.size() > 16384 || value.reference.id != manifest.id ||
        value.reference.version != manifest.version || value.reference.digest != PackageDigest(value) || !Neutral(value))
        throw std::runtime_error("Package content/digest or disclosure check failed.");
}

void WritePackage(const std::filesystem::path& path, const SkillPackage& value)
{
    storage::CheckPath(path);
    std::filesystem::create_directories(path);
    storage::AtomicWrite(path / "manifest.json", ManifestJson(value.manifest).dump(2));
    storage::AtomicWrite(path / "SKILL.md", value.procedure);
    storage::AtomicWrite(path / "cases.json", CasesJson(value.cases).dump(2));
}
}

bool LoadSkillPackage(const std::filesystem::path& path, SkillPackage& result, std::string& error)
{
    try
    {
        storage::CheckPath(path);
        std::size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(path))
        {
            const auto name = entry.path().filename().string();
            if ((name != "manifest.json" && name != "SKILL.md" && name != "cases.json") || !entry.is_regular_file())
                throw std::runtime_error("Unreviewed package asset or helper.");
            ++count;
        }
        if (count != 3)
            throw std::runtime_error("Package assets incomplete.");
        SkillPackage package;
        package.manifest = DecodeManifest(storage::ReadJson(path / "manifest.json", 16384));
        package.procedure = storage::Read(path / "SKILL.md", 16384);
        package.cases = DecodeCases(storage::ReadJson(path / "cases.json", 65536));
        package.reference = {package.manifest.id, package.manifest.version, PackageDigest(package)};
        ValidatePackage(package);
        result = std::move(package);
        error.clear();
        return true;
    }
    catch (...)
    {
        error = "Skill package is invalid, unsafe or incomplete.";
        return false;
    }
}

bool RequiresCapability(const SkillPin& pin, const actions::ActionType operation)
{
    return std::find(pin.manifest.requiredCapabilities.begin(), pin.manifest.requiredCapabilities.end(), operation) !=
               pin.manifest.requiredCapabilities.end() ||
           std::find(pin.manifest.optionalCapabilities.begin(), pin.manifest.optionalCapabilities.end(), operation) !=
               pin.manifest.optionalCapabilities.end();
}

SkillInventoryResult SummarizeInventory(const SkillPin& pin, const std::vector<std::string>& entries)
{
    SkillInventoryResult result;
    try
    {
        ValidatePackage(pin);
        if (entries.size() > 10001)
            throw std::runtime_error("Inventory output is oversized.");
        result.complete = true;
        for (const auto& entry : entries)
        {
            if (entry.size() > 1024 || entry.find('\n') != std::string::npos || entry.find('\r') != std::string::npos)
                throw std::runtime_error("Inventory entry is invalid.");
            if (entry == "[LIMIT] Additional entries were omitted.")
            {
                result.complete = false;
                continue;
            }
            const auto marker = entry.find(']');
            if (marker == std::string::npos || marker + 1 >= entry.size() || !std::isspace(static_cast<unsigned char>(entry[marker + 1])) ||
                entry.find_first_not_of(" \t", marker + 1) == std::string::npos)
                throw std::runtime_error("Unrecognized inventory entry.");
            const auto kind = entry.substr(0, marker + 1);
            if (kind == "[FILE]")
                ++result.files;
            else if (kind == "[DIR]")
                ++result.directories;
            else if (kind == "[LINK]")
                ++result.links;
            else if (kind == "[OTHER]")
                ++result.other;
            else
                throw std::runtime_error("Unrecognized inventory type.");
            ++result.entries;
        }
        std::ostringstream summary;
        summary << result.entries << " entries";
        if (pin.manifest.outputFormat == "typed-counts")
            summary << ": " << result.files << " files, " << result.directories << " directories, " << result.links << " links, "
                    << result.other << " other";
        summary << '.';
        if (!result.complete)
            summary << " Listing incomplete; additional entries were omitted.";
        result.summary = summary.str();
        result.verified = true;
    }
    catch (...)
    {
        result.verified = false;
        result.complete = false;
        result.summary = "Inventory output did not satisfy the pinned package contract.";
    }
    return result;
}

SkillVerification VerifySkillPackage(const SkillPackage& package, const std::vector<SkillCase>& fresh)
{
    SkillVerification result;
    try
    {
        ValidatePackage(package);
        (void)DecodeCases(CasesJson(fresh));
        std::set<std::string> inputs, ids;
        const auto run = [&](const SkillCase& entry, const bool freshCase)
        {
            auto canonicalEntries = entry.entries;
            std::sort(canonicalEntries.begin(), canonicalEntries.end());
            const std::string fingerprint = audit::ContentDigest(json(canonicalEntries).dump());
            if (!ids.insert(entry.id).second || !inputs.insert(fingerprint).second)
                throw std::runtime_error("Duplicate validation case.");
            const auto observed = SummarizeInventory(package, entry.entries);
            if (!observed.verified || observed.summary != entry.expectedSummary)
                throw std::runtime_error("Validation case failed.");
            if (freshCase)
                ++result.freshCases;
            else
                ++result.regressionCases;
        };
        for (const auto& entry : package.cases)
            run(entry, false);
        for (const auto& entry : fresh)
            run(entry, true);
        result.packageDigest = package.reference.digest;
        result.evidenceDigest = audit::ContentDigest(
            json{{"packageDigest", result.packageDigest}, {"regression", CasesJson(package.cases)}, {"fresh", CasesJson(fresh)}}.dump());
        result.freshEvidence = fresh;
        result.passed = result.regressionCases > 0 && result.freshCases > 0;
    }
    catch (...)
    {
        result.passed = false;
    }
    return result;
}

SkillPackageStore::SkillPackageStore(std::filesystem::path value, std::filesystem::path seeds)
    : directory(std::move(value)), seedDirectory(std::move(seeds))
{
}

bool SkillPackageStore::SaveState(std::string& error) const
{
    try
    {
        json reviews = json::object();
        for (const auto& [key, review] : exportReviews)
            reviews[key] = {{"packageDigest", review.packageDigest}, {"evidenceDigest", review.evidenceDigest},
                {"syntheticExamplesReviewed", review.syntheticExamplesReviewed}, {"disclosureReviewed", review.disclosureReviewed},
                {"neutralProcedureApproved", review.neutralProcedureApproved}};
        storage::AtomicWrite(directory / "selection.json",
            json{{"schemaVersion", 1}, {"active", active}, {"previous", previous}, {"exportReviews", reviews}}.dump(2));
        error.clear();
        return true;
    }
    catch (...)
    {
        error = "Skill selection could not be saved safely.";
        return false;
    }
}

bool SkillPackageStore::Initialize(std::string& error)
{
    const std::lock_guard lock(mutex);
    try
    {
        storage::CheckPath(directory);
        storage::CheckPath(seedDirectory);
        std::map<std::string, SkillPackage> loaded;
        std::set<std::string> trustedSeeds;
        const auto scan = [&](const std::filesystem::path& root)
        {
            if (!std::filesystem::exists(root))
                return;
            std::size_t count = 0;
            for (const auto& id : std::filesystem::directory_iterator(root))
            {
                if (!Identifier(id.path().filename().string()) || !id.is_directory())
                    throw std::runtime_error("Invalid package identity directory.");
                for (const auto& version : std::filesystem::directory_iterator(id.path()))
                {
                    if (++count > 128 || !Version(version.path().filename().string()) || !version.is_directory())
                        throw std::runtime_error("Package count/version invalid.");
                    SkillPackage package;
                    if (!LoadSkillPackage(version.path(), package, error) || package.reference.id != id.path().filename().string() ||
                        package.reference.version != version.path().filename().string())
                        throw std::runtime_error("Invalid package directory/content.");
                    const auto key = Key(package.reference);
                    if (loaded.contains(key) && loaded.at(key).reference.digest != package.reference.digest)
                        throw std::runtime_error("Immutable package version changed.");
                    loaded[key] = std::move(package);
                }
            }
        };
        scan(seedDirectory);
        for (const auto& [key, package] : loaded)
            trustedSeeds.insert(key);
        scan(directory / "packages");
        std::map<std::string, std::string> selected;
        std::map<std::string, std::vector<std::string>> history;
        std::map<std::string, SkillExportReview> reviews;
        if (std::filesystem::exists(directory / "selection.json"))
        {
            const auto state = storage::ReadJson(directory / "selection.json");
            ExactKeys(state, {"schemaVersion", "active", "previous", "exportReviews"});
            if (state.at("schemaVersion") != 1)
                throw std::runtime_error("Invalid skill selection schema.");
            selected = state.at("active").get<std::map<std::string, std::string>>();
            history = state.at("previous").get<std::map<std::string, std::vector<std::string>>>();
            if (selected.size() > 64 || history.size() > 64 || state.at("exportReviews").size() > 128)
                throw std::runtime_error("Skill history capacity exceeded.");
            for (const auto& [id, key] : selected)
                if (!Identifier(id) || !loaded.contains(key) || loaded.at(key).reference.id != id)
                    throw std::runtime_error("Unknown active skill.");
            for (const auto& [id, values] : history)
            {
                if (!Identifier(id) || values.size() > 32)
                    throw std::runtime_error("Invalid skill rollback history.");
                for (const auto& key : values)
                    if (!loaded.contains(key) || loaded.at(key).reference.id != id)
                        throw std::runtime_error("Missing rollback version.");
            }
            for (const auto& item : state.at("exportReviews").items())
            {
                const auto& value = item.value();
                SkillExportReview review{value.at("packageDigest").get<std::string>(), value.at("evidenceDigest").get<std::string>(),
                    value.at("syntheticExamplesReviewed").get<bool>(), value.at("disclosureReviewed").get<bool>(),
                    value.at("neutralProcedureApproved").get<bool>()};
                if (!loaded.contains(item.key()) || review.packageDigest != loaded.at(item.key()).reference.digest ||
                    review.evidenceDigest.size() != 64 || !review.syntheticExamplesReviewed || !review.disclosureReviewed ||
                    !review.neutralProcedureApproved)
                    throw std::runtime_error("Export decision is invalid.");
                reviews[item.key()] = std::move(review);
            }
            for (const auto& [id, key] : selected)
                if (!trustedSeeds.contains(key) && !reviews.contains(key))
                    throw std::runtime_error("Selected private package lacks review.");
        }
        else
        {
            for (const auto& [key, package] : loaded)
                if (package.reference.version == "1.0.0" && trustedSeeds.contains(key))
                    selected[package.reference.id] = key;
        }
        const auto oldPackages = packages;
        const auto oldActive = active;
        const auto oldPrevious = previous;
        const auto oldReviews = exportReviews;
        packages = std::move(loaded);
        active = std::move(selected);
        previous = std::move(history);
        exportReviews = std::move(reviews);
        if (!SaveState(error))
        {
            packages = oldPackages;
            active = oldActive;
            previous = oldPrevious;
            exportReviews = oldReviews;
            return false;
        }
        initialized = true;
        return true;
    }
    catch (...)
    {
        error = "Skill packages or saved selection are invalid.";
        return false;
    }
}

bool SkillPackageStore::Publish(
    const SkillPackage& package, const SkillVerification& verification, const SkillExportReview& review, std::string& error)
{
    const std::lock_guard lock(mutex);
    try
    {
        ValidatePackage(package);
        const auto checked = VerifySkillPackage(package, verification.freshEvidence);
        if (!initialized || packages.size() >= 128 || !checked.passed || !verification.passed ||
            checked.packageDigest != verification.packageDigest || checked.evidenceDigest != verification.evidenceDigest ||
            checked.freshCases != verification.freshCases || checked.regressionCases != verification.regressionCases ||
            review.packageDigest != package.reference.digest || review.evidenceDigest != checked.evidenceDigest ||
            !review.syntheticExamplesReviewed || !review.disclosureReviewed || !review.neutralProcedureApproved)
            throw std::runtime_error("Package objective/privacy/export checks are incomplete.");
        const auto key = Key(package.reference);
        if (packages.contains(key) && packages.at(key).reference.digest != package.reference.digest)
            throw std::runtime_error("Published version is immutable.");
        const auto target = directory / "packages" / package.reference.id / package.reference.version;
        if (!std::filesystem::exists(target))
        {
            const auto staging = directory / "staging" / package.reference.digest;
            WritePackage(staging, package);
            std::filesystem::create_directories(target.parent_path());
            storage::CheckPath(target);
            std::filesystem::rename(staging, target);
        }
        else
        {
            SkillPackage existing;
            if (!LoadSkillPackage(target, existing, error) || existing.reference.digest != package.reference.digest)
                throw std::runtime_error("Published version drifted.");
        }
        const auto oldPackages = packages;
        const auto oldReviews = exportReviews;
        packages[key] = package;
        exportReviews[key] = review;
        if (!SaveState(error))
        {
            packages = oldPackages;
            exportReviews = oldReviews;
            return false;
        }
        return true;
    }
    catch (...)
    {
        error = "Skill publication failed its immutable validation or export decision.";
        return false;
    }
}

bool SkillPackageStore::Activate(const SkillPackageReference& reference, std::string& error)
{
    if (!Identifier(reference.id) || !Version(reference.version) || reference.digest.size() != 64 ||
        !std::all_of(reference.digest.begin(), reference.digest.end(),
            [](const unsigned char c) { return std::isdigit(c) || (c >= 'a' && c <= 'f'); }))
    {
        error = "Skill reference is invalid.";
        return false;
    }
    const std::lock_guard lock(mutex);
    const auto key = Key(reference);
    SkillPackage seed;
    std::string seedError;
    const bool trustedSeed =
        LoadSkillPackage(seedDirectory / reference.id / reference.version, seed, seedError) && seed.reference.digest == reference.digest;
    if (!initialized || !packages.contains(key) || packages.at(key).reference.digest != reference.digest ||
        (!trustedSeed && !exportReviews.contains(key)) || (reference.version != "1.0.0" && !exportReviews.contains(key)))
    {
        error = "Skill version has no eligible acceptance.";
        return false;
    }
    const auto oldActive = active;
    const auto oldPrevious = previous;
    if (active.contains(reference.id) && active[reference.id] != key)
    {
        if (previous[reference.id].size() >= 32)
        {
            error = "Skill rollback history is full.";
            return false;
        }
        previous[reference.id].push_back(active[reference.id]);
    }
    active[reference.id] = key;
    if (!SaveState(error))
    {
        active = oldActive;
        previous = oldPrevious;
        return false;
    }
    return true;
}

bool SkillPackageStore::Rollback(const std::string& id, std::string& error)
{
    const std::lock_guard lock(mutex);
    if (!initialized || !previous.contains(id) || previous[id].empty())
    {
        error = "No earlier accepted skill version is available.";
        return false;
    }
    const auto oldActive = active;
    const auto oldPrevious = previous;
    active[id] = previous[id].back();
    previous[id].pop_back();
    if (!SaveState(error))
    {
        active = oldActive;
        previous = oldPrevious;
        return false;
    }
    return true;
}

std::optional<SkillPin> SkillPackageStore::Pin(const std::string& id) const
{
    const std::lock_guard lock(mutex);
    if (!initialized || !active.contains(id))
        return std::nullopt;
    return packages.at(active.at(id));
}

std::optional<SkillPin> SkillPackageStore::LoadPinned(const SkillPackageReference& reference) const
{
    const std::lock_guard lock(mutex);
    const auto key = Key(reference);
    if (!initialized || !packages.contains(key) || packages.at(key).reference.digest != reference.digest)
        return std::nullopt;
    return packages.at(key);
}

bool SkillPackageStore::Export(const SkillPackageReference& reference, const std::filesystem::path& destination, std::string& error) const
{
    const std::lock_guard lock(mutex);
    try
    {
        const auto key = Key(reference);
        const auto relative = destination.lexically_normal().lexically_relative((directory / "exports").lexically_normal());
        if (!initialized || !packages.contains(key) || packages.at(key).reference.digest != reference.digest ||
            !exportReviews.contains(key) || relative.empty() || relative.is_absolute())
            throw std::runtime_error("Export must target the private reviewed export area.");
        for (const auto& part : relative)
            if (part == "..")
                throw std::runtime_error("Export escaped its private artifact area.");
        storage::CheckPath(destination);
        if (std::filesystem::exists(destination))
        {
            SkillPackage existing;
            if (!LoadSkillPackage(destination, existing, error) || existing.reference.digest != reference.digest)
                throw std::runtime_error("Export destination contains different data.");
            return true;
        }
        const auto staging = destination.parent_path() / (".export-" + reference.digest);
        WritePackage(staging, packages.at(key));
        std::filesystem::rename(staging, destination);
        error.clear();
        return true;
    }
    catch (...)
    {
        error = "Skill export was not eligible or could not be published safely.";
        return false;
    }
}

} // namespace revia::skills
