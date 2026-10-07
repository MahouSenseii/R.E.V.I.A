#include "Evaluation/campaignManifest.h"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace revia::evaluation
{

namespace
{

bool Hex(const std::string& value, std::size_t size)
{
    return value.size() == size &&
           std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool Present(const std::string& value)
{
    return value.find_first_not_of(" \t\r\n") != std::string::npos;
}

std::uint64_t Unsigned(const nlohmann::json& object, const char* field)
{
    const auto& value = object.at(field);
    if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>() < 0))
    {
        throw std::runtime_error(std::string(field) + " must be an unsigned integer.");
    }
    return value.get<std::uint64_t>();
}

}

bool ValidateCampaignManifest(const CampaignManifest& manifest, std::string& error)
{
    error.clear();
    if (manifest.schemaVersion != 1)
    {
        error = "schemaVersion is unsupported.";
    }
    else if (!Present(manifest.campaignId))
    {
        error = "campaignId is required.";
    }
    else if (!Hex(manifest.commit, 40) && !Hex(manifest.commit, 64))
    {
        error = "commit must identify the source parent.";
    }
    else if (!Hex(manifest.sourceDigest, 64))
    {
        error = "sourceDigest is required even for clean source.";
    }
    else if (!Hex(manifest.buildDigest, 64))
    {
        error = "buildDigest is required.";
    }
    else if ((manifest.providerAvailable && !Hex(manifest.providerDigest, 64)) ||
             (!manifest.providerAvailable && !manifest.providerDigest.empty()))
    {
        error = "providerDigest must bind an available provider, or be empty when unavailable.";
    }
    else if (!Hex(manifest.settingsDigest, 64))
    {
        error = "settingsDigest is required.";
    }
    else if (!Hex(manifest.fixtureDigest, 64))
    {
        error = "fixtureDigest is required.";
    }
    else if (!Present(manifest.oracleVersion))
    {
        error = "oracleVersion is required.";
    }
    else if (!Present(manifest.hardware))
    {
        error = "hardware is required.";
    }
    else if (!Present(manifest.timingBoundary))
    {
        error = "timingBoundary is required.";
    }
    return error.empty();
}

std::vector<std::string> CampaignIdentityMismatches(const CampaignManifest& expected, const CampaignManifest& candidate)
{
    std::vector<std::string> fields;
#define REVIA_COMPARE_CAMPAIGN(field)                                                                                                      \
    if (expected.field != candidate.field)                                                                                                 \
    fields.emplace_back(#field)
    REVIA_COMPARE_CAMPAIGN(schemaVersion);
    REVIA_COMPARE_CAMPAIGN(campaignId);
    REVIA_COMPARE_CAMPAIGN(commit);
    REVIA_COMPARE_CAMPAIGN(sourceDigest);
    REVIA_COMPARE_CAMPAIGN(sourceDirty);
    REVIA_COMPARE_CAMPAIGN(buildDigest);
    REVIA_COMPARE_CAMPAIGN(providerDigest);
    REVIA_COMPARE_CAMPAIGN(providerAvailable);
    REVIA_COMPARE_CAMPAIGN(settingsDigest);
    REVIA_COMPARE_CAMPAIGN(fixtureDigest);
    REVIA_COMPARE_CAMPAIGN(oracleVersion);
    REVIA_COMPARE_CAMPAIGN(hardware);
    REVIA_COMPARE_CAMPAIGN(seed);
    REVIA_COMPARE_CAMPAIGN(timingBoundary);
#undef REVIA_COMPARE_CAMPAIGN
    return fields;
}

std::string SerializeCampaignManifest(const CampaignManifest& manifest)
{
    std::string error;
    if (!ValidateCampaignManifest(manifest, error))
    {
        throw std::invalid_argument(error);
    }
    return nlohmann::json{{"schemaVersion", manifest.schemaVersion}, {"campaignId", manifest.campaignId}, {"commit", manifest.commit},
        {"sourceDigest", manifest.sourceDigest}, {"sourceDirty", manifest.sourceDirty}, {"buildDigest", manifest.buildDigest},
        {"providerDigest", manifest.providerDigest}, {"providerAvailable", manifest.providerAvailable},
        {"settingsDigest", manifest.settingsDigest}, {"fixtureDigest", manifest.fixtureDigest}, {"oracleVersion", manifest.oracleVersion},
        {"hardware", manifest.hardware}, {"seed", manifest.seed}, {"timingBoundary", manifest.timingBoundary}}
        .dump(2);
}

bool ParseCampaignManifest(const std::string& bytes, CampaignManifest& manifest, std::string& error)
{
    try
    {
        const auto object = nlohmann::json::parse(bytes);
        CampaignManifest parsed;
        const auto version = Unsigned(object, "schemaVersion");
        if (version > std::numeric_limits<std::uint32_t>::max())
        {
            throw std::runtime_error("schemaVersion is out of range.");
        }
        parsed.schemaVersion = static_cast<std::uint32_t>(version);
        parsed.campaignId = object.at("campaignId").get<std::string>();
        parsed.commit = object.at("commit").get<std::string>();
        parsed.sourceDigest = object.at("sourceDigest").get<std::string>();
        parsed.sourceDirty = object.at("sourceDirty").get<bool>();
        parsed.buildDigest = object.at("buildDigest").get<std::string>();
        parsed.providerDigest = object.at("providerDigest").get<std::string>();
        parsed.providerAvailable = object.at("providerAvailable").get<bool>();
        parsed.settingsDigest = object.at("settingsDigest").get<std::string>();
        parsed.fixtureDigest = object.at("fixtureDigest").get<std::string>();
        parsed.oracleVersion = object.at("oracleVersion").get<std::string>();
        parsed.hardware = object.at("hardware").get<std::string>();
        parsed.seed = Unsigned(object, "seed");
        parsed.timingBoundary = object.at("timingBoundary").get<std::string>();
        if (!ValidateCampaignManifest(parsed, error))
        {
            return false;
        }
        manifest = std::move(parsed);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool WriteCampaignManifestOnce(const std::filesystem::path& path, const CampaignManifest& manifest, std::string& error)
{
    if (!ValidateCampaignManifest(manifest, error))
    {
        return false;
    }
    const auto bytes = SerializeCampaignManifest(manifest) + '\n';
#ifdef _WIN32
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = "Manifest already exists or cannot be created.";
        return false;
    }
    DWORD written = 0;
    const bool valid = bytes.size() <= std::numeric_limits<DWORD>::max() &&
                       WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() &&
                       FlushFileBuffers(file);
    const bool closed = CloseHandle(file) != FALSE;
#else
    const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file < 0)
    {
        error = "Manifest already exists or cannot be created.";
        return false;
    }
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const auto count = write(file, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0)
        {
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    const bool valid = offset == bytes.size() && fsync(file) == 0;
    const bool closed = close(file) == 0;
#endif
    if (!valid || !closed)
    {
        error = "Manifest persistence failed; the partial file remains unavailable evidence.";
    }
    return valid && closed;
}

}
