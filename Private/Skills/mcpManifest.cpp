#include "Skills/mcpManifest.h"

#include "../Computer/artifactDigest.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>

namespace revia::skills
{

namespace
{
using json = nlohmann::json;

bool PlainName(const std::string& value, const std::size_t maximum)
{
    if (value.empty() || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char character)
    {
        return std::isalnum(character) || character == '_' || character == '-' || character == '.';
    });
}
} // namespace

const McpToolPin* McpManifest::FindTool(const std::string& name) const
{
    for (const McpToolPin& tool : tools)
    {
        if (tool.name == name) return &tool;
    }
    return nullptr;
}

std::string ToolDigest(const std::string& description, const std::string& inputSchema)
{
    return revia::computer::Sha256Hex(description + "\n" + inputSchema);
}

bool ValidToolName(const std::string& name)
{
    return PlainName(name, 128);
}

bool ValidServerId(const std::string& id)
{
    return PlainName(id, 64);
}

bool LoadManifest(const std::filesystem::path& path, McpManifest& outManifest, std::string& outError)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        outError = "Could not open " + path.string() + ".";
        return false;
    }
    json data;
    try
    {
        file >> data;
    }
    catch (const std::exception& error)
    {
        outError = path.string() + " is not JSON: " + error.what();
        return false;
    }
    if (!data.is_object())
    {
        outError = path.string() + " must hold one JSON object.";
        return false;
    }
    McpManifest manifest;
    manifest.path = path;
    manifest.id = data.value("id", path.stem().string());
    if (!ValidServerId(manifest.id))
    {
        outError = path.string() + ": \"" + manifest.id + "\" is not a server id (letters, digits, '_', '-', '.').";
        return false;
    }
    manifest.enabled = data.value("enabled", false);
    const json transport = data.value("transport", json::object());
    manifest.transportKind = transport.value("kind", "http");
    manifest.url = transport.value("url", "");
    if (manifest.transportKind != "http")
    {
        outError = path.string() + ": transport.kind must be \"http\"; a stdio server goes through Tools/Mcp/bridge.mjs.";
        return false;
    }
    if (manifest.url.rfind("http://", 0) != 0 && manifest.url.rfind("https://", 0) != 0)
    {
        outError = path.string() + ": transport.url must start with http:// or https://.";
        return false;
    }
    if (transport.contains("headers") && transport["headers"].is_object())
    {
        for (const auto& [name, value] : transport["headers"].items())
        {
            if (value.is_string()) manifest.headers.emplace_back(name, value.get<std::string>());
        }
    }
    for (const json& item : data.value("tools", json::array()))
    {
        if (!item.is_object()) continue;
        McpToolPin tool;
        tool.name = item.value("name", "");
        if (!ValidToolName(tool.name))
        {
            outError = path.string() + ": a tool name is missing or not plain.";
            return false;
        }
        tool.description = item.value("description", "");
        tool.inputSchema = item.contains("inputSchema") && item["inputSchema"].is_object()
            ? item["inputSchema"].dump() : item.value("inputSchema", "");
        tool.digest = item.value("digest", "");
        tool.risk = actions::RiskLevelFromString(item.value("risk", "destructive"));
        tool.enabled = item.value("enabled", true);
        manifest.tools.push_back(std::move(tool));
    }
    outManifest = std::move(manifest);
    return true;
}

bool SaveManifest(const McpManifest& manifest, std::string& outError)
{
    json data;
    data["id"] = manifest.id;
    data["enabled"] = manifest.enabled;
    json transport;
    transport["kind"] = manifest.transportKind;
    transport["url"] = manifest.url;
    if (!manifest.headers.empty())
    {
        json headers = json::object();
        for (const auto& [name, value] : manifest.headers) headers[name] = value;
        transport["headers"] = headers;
    }
    data["transport"] = transport;
    json tools = json::array();
    for (const McpToolPin& tool : manifest.tools)
    {
        json item;
        item["name"] = tool.name;
        item["description"] = tool.description;
        try
        {
            item["inputSchema"] = tool.inputSchema.empty() ? json::object() : json::parse(tool.inputSchema);
        }
        catch (const std::exception&)
        {
            item["inputSchema"] = tool.inputSchema;
        }
        item["digest"] = tool.digest;
        item["risk"] = actions::ToString(tool.risk);
        item["enabled"] = tool.enabled;
        tools.push_back(std::move(item));
    }
    data["tools"] = tools;
    std::error_code error;
    std::filesystem::create_directories(manifest.path.parent_path(), error);
    std::ofstream file(manifest.path, std::ios::trunc);
    if (!file.is_open())
    {
        outError = "Could not write " + manifest.path.string() + ".";
        return false;
    }
    file << data.dump(2) << '\n';
    return file.good();
}

std::vector<McpManifest> LoadManifestDirectory(
    const std::filesystem::path& directory, std::vector<std::string>& outErrors)
{
    std::vector<McpManifest> manifests;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return manifests;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path& file : files)
    {
        McpManifest manifest;
        std::string loadError;
        if (LoadManifest(file, manifest, loadError)) manifests.push_back(std::move(manifest));
        else outErrors.push_back(loadError);
    }
    return manifests;
}

} // namespace revia::skills
