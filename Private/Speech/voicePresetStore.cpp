#include "Speech/voicePresetStore.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace revia::speech
{

namespace
{
std::string StringValue(const nlohmann::json& value, const char* key)
{
    return value.contains(key) && value[key].is_string()
        ? value[key].get<std::string>()
        : std::string();
}

VoicePreset ParsePreset(const nlohmann::json& value)
{
    VoicePreset preset;
    preset.id = StringValue(value, "id");
    preset.name = StringValue(value, "name");
    preset.description = StringValue(value, "description");
    preset.language = StringValue(value, "language");
    preset.referenceText = StringValue(value, "referenceText");
    preset.referenceAudioPath = StringValue(value, "referenceAudioPath");
    preset.createdAt = StringValue(value, "createdAt");
    if (preset.language.empty())
    {
        preset.language = "English";
    }
    return preset;
}

nlohmann::json SerializePreset(const VoicePreset& preset)
{
    return {
        {"id", preset.id},
        {"name", preset.name},
        {"description", preset.description},
        {"language", preset.language},
        {"referenceText", preset.referenceText},
        {"referenceAudioPath", preset.referenceAudioPath},
        {"createdAt", preset.createdAt}
    };
}
}

VoicePresetStore::VoicePresetStore(std::filesystem::path root)
    : rootDirectory(std::move(root))
{
}

void VoicePresetStore::SetRoot(std::filesystem::path root)
{
    std::lock_guard lock(mutex);
    rootDirectory = std::move(root);
}

std::filesystem::path VoicePresetStore::Root() const
{
    std::lock_guard lock(mutex);
    return rootDirectory;
}

std::vector<VoicePreset> VoicePresetStore::List() const
{
    std::lock_guard lock(mutex);
    nlohmann::json document;
    std::string error;
    if (!LoadDocument(document, error) || !document["presets"].is_array())
    {
        return {};
    }

    std::vector<VoicePreset> presets;
    for (const auto& item : document["presets"])
    {
        if (!item.is_object())
        {
            continue;
        }
        VoicePreset preset = ParsePreset(item);
        if (IsSafeId(preset.id) && !preset.name.empty() &&
            !preset.referenceAudioPath.empty())
        {
            presets.push_back(std::move(preset));
        }
    }
    std::sort(presets.begin(), presets.end(), [](const VoicePreset& left, const VoicePreset& right)
    {
        return left.name < right.name;
    });
    return presets;
}

std::optional<VoicePreset> VoicePresetStore::Find(const std::string& presetId) const
{
    const auto presets = List();
    const auto found = std::find_if(presets.begin(), presets.end(), [&](const VoicePreset& preset)
    {
        return preset.id == presetId;
    });
    return found == presets.end() ? std::nullopt : std::optional<VoicePreset>(*found);
}

bool VoicePresetStore::Save(const VoicePreset& preset, std::string& outError)
{
    std::lock_guard lock(mutex);
    if (!IsSafeId(preset.id) || preset.name.empty() || preset.description.empty() ||
        preset.referenceText.empty() || preset.referenceAudioPath.empty())
    {
        outError = "The voice preset is incomplete or has an unsafe id.";
        return false;
    }
    const std::filesystem::path reference = preset.referenceAudioPath;
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(reference, fileError))
    {
        outError = "The generated reference audio was not found.";
        return false;
    }

    nlohmann::json document;
    if (!LoadDocument(document, outError))
    {
        return false;
    }
    auto& presets = document["presets"];
    bool replaced = false;
    for (auto& item : presets)
    {
        if (item.is_object() && StringValue(item, "id") == preset.id)
        {
            item = SerializePreset(preset);
            replaced = true;
            break;
        }
    }
    if (!replaced)
    {
        presets.push_back(SerializePreset(preset));
    }
    return SaveDocument(document, outError);
}

bool VoicePresetStore::Assign(
    const std::string& profileId,
    const std::string& presetId,
    std::string& outError)
{
    std::lock_guard lock(mutex);
    if (!IsSafeId(profileId) || (!presetId.empty() && !IsSafeId(presetId)))
    {
        outError = "The profile or voice preset id is unsafe.";
        return false;
    }

    nlohmann::json document;
    if (!LoadDocument(document, outError))
    {
        return false;
    }
    if (!presetId.empty())
    {
        const auto& presets = document["presets"];
        const bool exists = std::any_of(presets.begin(), presets.end(), [&](const auto& item)
        {
            return item.is_object() && StringValue(item, "id") == presetId;
        });
        if (!exists)
        {
            outError = "The selected voice preset does not exist.";
            return false;
        }
        document["profileAssignments"][profileId] = presetId;
    }
    else
    {
        document["profileAssignments"].erase(profileId);
    }
    return SaveDocument(document, outError);
}

std::string VoicePresetStore::AssignedPresetId(const std::string& profileId) const
{
    std::lock_guard lock(mutex);
    nlohmann::json document;
    std::string error;
    if (!LoadDocument(document, error))
    {
        return {};
    }
    const auto& assignments = document["profileAssignments"];
    return assignments.contains(profileId) && assignments[profileId].is_string()
        ? assignments[profileId].get<std::string>()
        : std::string();
}

bool VoicePresetStore::IsSafeId(const std::string& value)
{
    return !value.empty() && value.size() <= 64 &&
        std::all_of(value.begin(), value.end(), [](const unsigned char character)
        {
            return std::isalnum(character) || character == '-' || character == '_';
        });
}

bool VoicePresetStore::LoadDocument(
    nlohmann::json& outDocument,
    std::string& outError) const
{
    outError.clear();
    outDocument = {
        {"version", 1},
        {"presets", nlohmann::json::array()},
        {"profileAssignments", nlohmann::json::object()}
    };
    const std::filesystem::path path = rootDirectory / "voices.json";
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        return true;
    }
    try
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            outError = "Could not open the voice preset database.";
            return false;
        }
        file >> outDocument;
        if (!outDocument.is_object() || !outDocument.contains("presets") ||
            !outDocument["presets"].is_array() ||
            !outDocument.contains("profileAssignments") ||
            !outDocument["profileAssignments"].is_object())
        {
            outError = "The voice preset database is malformed.";
            return false;
        }
    }
    catch (const std::exception& exception)
    {
        outError = std::string("Could not parse the voice preset database: ") + exception.what();
        return false;
    }
    return true;
}

bool VoicePresetStore::SaveDocument(
    const nlohmann::json& document,
    std::string& outError) const
{
    std::error_code error;
    std::filesystem::create_directories(rootDirectory, error);
    if (error)
    {
        outError = "Could not create the voice data directory: " + error.message();
        return false;
    }
    const std::filesystem::path path = rootDirectory / "voices.json";
    const std::filesystem::path temporary = rootDirectory / "voices.json.tmp";
    try
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file.is_open())
        {
            outError = "Could not write the voice preset database.";
            return false;
        }
        file << std::setw(2) << document << '\n';
        file.close();
        if (!file.good())
        {
            outError = "The voice preset database write did not complete.";
            return false;
        }
        std::filesystem::rename(temporary, path, error);
        if (error)
        {
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
        }
        if (error)
        {
            outError = "Could not replace the voice preset database: " + error.message();
            return false;
        }
    }
    catch (const std::exception& exception)
    {
        outError = std::string("Could not save the voice preset database: ") + exception.what();
        return false;
    }
    return true;
}

} // namespace revia::speech
