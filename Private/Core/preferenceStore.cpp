#include "Core/preferenceStore.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>
#include <utility>

namespace revia::core
{

namespace
{

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool ParseBoolean(const std::string& value, bool& out)
{
    const std::string lowered = Lower(value);
    if (lowered == "on" || lowered == "true" || lowered == "yes" || lowered == "1")
    {
        out = true;
        return true;
    }
    if (lowered == "off" || lowered == "false" || lowered == "no" || lowered == "0")
    {
        out = false;
        return true;
    }
    return false;
}

bool ParseNumber(const std::string& value, double& out)
{
    try
    {
        std::size_t consumed = 0;
        out = std::stod(value, &consumed);
        return consumed == value.size();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

std::string TypeName(const PreferenceType type)
{
    switch (type)
    {
        case PreferenceType::Boolean: return "on/off";
        case PreferenceType::Integer: return "whole number";
        case PreferenceType::Decimal: return "number";
        case PreferenceType::Text: return "text";
    }
    return "value";
}

} // namespace

const std::vector<PreferenceKey>& PreferenceStore::Writable()
{
    // Deliberately short. Every entry here is a comfort or pacing setting whose worst
    // case is an annoying Revia; nothing here changes what she is permitted to do.
    static const std::vector<PreferenceKey> keys = {
        {"speech.enabled", PreferenceType::Boolean,
            "Read replies aloud.", 0, 0, {}},
        {"speech.volume", PreferenceType::Integer,
            "Speech volume.", 0, 100, {}},
        {"speech.rate", PreferenceType::Integer,
            "Speech rate.", -10, 10, {}},
        {"speech.speakGreeting", PreferenceType::Boolean,
            "Speak the greeting when Revia comes online.", 0, 0, {}},
        {"speechRecognition.enabled", PreferenceType::Boolean,
            "Accept microphone input.", 0, 0, {}},
        {"bargeIn.enabled", PreferenceType::Boolean,
            "Let speaking over Revia interrupt her.", 0, 0, {}},
        {"initiative.enabled", PreferenceType::Boolean,
            "Allow Revia to start a conversation.", 0, 0, {}},
        {"initiative.maxPerHour", PreferenceType::Integer,
            "Ceiling on unprompted openings per hour.", 0, 60, {}},
        {"llm.temperature", PreferenceType::Decimal,
            "Sampling temperature for replies.", 0.0, 2.0, {}},
        {"resources.usageSampleSeconds", PreferenceType::Integer,
            "How often live resource usage is sampled; 0 turns it off.", 0, 3600, {}},
        {"conversation.archiveEnabled", PreferenceType::Boolean,
            "Keep a durable, searchable record of conversations.", 0, 0, {}},
        {"activeProfile", PreferenceType::Text,
            "Which profile Revia loads at startup.", 0, 0, {}}
    };
    return keys;
}

const PreferenceKey* PreferenceStore::Find(const std::string& name)
{
    const std::string wanted = Lower(name);
    for (const PreferenceKey& key : Writable())
    {
        if (Lower(key.name) == wanted)
        {
            return &key;
        }
    }
    return nullptr;
}

bool PreferenceStore::IsAuthoritySetting(const std::string& name)
{
    static const std::vector<std::string> authorityPrefixes = {
        "capabilit", "approvedroot", "approvedapp", "approvedcontrol", "actions.",
        "policy.", "mode", "risk", "autoapprove", "internet", "vision", "perception",
        "permission", "root", "application", "control", "sandbox", "goal.scope"
    };
    const std::string lowered = Lower(name);
    return std::any_of(authorityPrefixes.begin(), authorityPrefixes.end(),
        [&lowered](const std::string& prefix)
        {
            return lowered.find(prefix) != std::string::npos;
        });
}

PreferenceStore::PreferenceStore(std::filesystem::path path)
    : storePath(std::move(path))
{
}

std::map<std::string, std::string> PreferenceStore::Load() const
{
    std::map<std::string, std::string> values;
    std::ifstream file(storePath);
    if (!file.is_open())
    {
        return values;
    }
    nlohmann::json document;
    try
    {
        file >> document;
    }
    catch (const std::exception&)
    {
        return values;
    }
    if (!document.is_object())
    {
        return values;
    }
    for (const auto& [key, value] : document.items())
    {
        // Filtered on read as well as on write. A hand-edited file must not be able to
        // introduce a key the writable table does not contain.
        if (Find(key) != nullptr && value.is_string())
        {
            values.emplace(key, value.get<std::string>());
        }
    }
    return values;
}

bool PreferenceStore::Write(const std::map<std::string, std::string>& values) const
{
    std::error_code error;
    if (!storePath.parent_path().empty())
    {
        std::filesystem::create_directories(storePath.parent_path(), error);
        if (error)
        {
            return false;
        }
    }

    nlohmann::json document = nlohmann::json::object();
    for (const auto& [key, value] : values)
    {
        document[key] = value;
    }

    // Written beside the target and moved into place, so an interrupted write leaves the
    // previous preferences intact rather than a half-file that parses as nothing.
    const std::filesystem::path temporary = storePath.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file.is_open())
        {
            return false;
        }
        file << document.dump(2) << '\n';
        if (!file.good())
        {
            return false;
        }
    }
    std::filesystem::rename(temporary, storePath, error);
    if (error)
    {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

PreferenceResult PreferenceStore::Set(const std::string& name, const std::string& value)
{
    PreferenceResult result;
    const PreferenceKey* key = Find(name);
    if (key == nullptr)
    {
        result.message = IsAuthoritySetting(name)
            ? "'" + name + "' decides what Revia is permitted to do, so it is not a "
              "preference. Change it in the Permissions tab or in capabilities.json, "
              "where the change is deliberate and audited."
            : "'" + name + "' is not a settable preference. Use /prefs to list them.";
        return result;
    }

    std::string stored;
    switch (key->type)
    {
        case PreferenceType::Boolean:
        {
            bool parsed = false;
            if (!ParseBoolean(value, parsed))
            {
                result.message = key->name + " takes on or off.";
                return result;
            }
            stored = parsed ? "true" : "false";
            break;
        }
        case PreferenceType::Integer:
        case PreferenceType::Decimal:
        {
            double parsed = 0.0;
            if (!ParseNumber(value, parsed))
            {
                result.message = key->name + " takes a " + TypeName(key->type) + '.';
                return result;
            }
            if (parsed < key->minimum || parsed > key->maximum)
            {
                std::ostringstream range;
                range << key->name << " must be between " << key->minimum << " and "
                    << key->maximum << '.';
                result.message = range.str();
                return result;
            }
            if (key->type == PreferenceType::Integer)
            {
                stored = std::to_string(static_cast<long long>(parsed));
            }
            else
            {
                std::ostringstream decimal;
                decimal << parsed;
                stored = decimal.str();
            }
            break;
        }
        case PreferenceType::Text:
        {
            if (value.empty())
            {
                result.message = key->name + " cannot be empty.";
                return result;
            }
            if (!key->allowed.empty() &&
                std::find(key->allowed.begin(), key->allowed.end(), value) ==
                    key->allowed.end())
            {
                result.message = key->name + " does not accept '" + value + "'.";
                return result;
            }
            stored = value;
            break;
        }
    }

    std::map<std::string, std::string> values = Load();
    values[key->name] = stored;
    if (!Write(values))
    {
        result.message = "The preference could not be saved to " + storePath.string() + '.';
        return result;
    }
    result.succeeded = true;
    result.message = key->name + " is now " + stored + ", and will stay that way.";
    return result;
}

PreferenceResult PreferenceStore::Clear(const std::string& name)
{
    PreferenceResult result;
    const PreferenceKey* key = Find(name);
    if (key == nullptr)
    {
        result.message = "'" + name + "' is not a settable preference.";
        return result;
    }
    std::map<std::string, std::string> values = Load();
    if (values.erase(key->name) == 0)
    {
        result.succeeded = true;
        result.message = key->name + " was already following the configured default.";
        return result;
    }
    if (!Write(values))
    {
        result.message = "The preference could not be cleared.";
        return result;
    }
    result.succeeded = true;
    result.message = key->name + " is back to the configured default.";
    return result;
}

void PreferenceStore::Apply(appSettings& settings) const
{
    const std::map<std::string, std::string> values = Load();
    const auto boolean = [&values](const std::string& name, bool& target)
    {
        const auto found = values.find(name);
        bool parsed = false;
        if (found != values.end() && ParseBoolean(found->second, parsed))
        {
            target = parsed;
        }
    };
    const auto integer = [&values](const std::string& name, int& target)
    {
        const auto found = values.find(name);
        double parsed = 0.0;
        if (found != values.end() && ParseNumber(found->second, parsed))
        {
            target = static_cast<int>(parsed);
        }
    };

    boolean("speech.enabled", settings.speech.bEnabled);
    integer("speech.volume", settings.speech.volume);
    integer("speech.rate", settings.speech.rate);
    boolean("speech.speakGreeting", settings.speech.bSpeakGreeting);
    boolean("speechRecognition.enabled", settings.speechRecognition.bEnabled);
    boolean("bargeIn.enabled", settings.bargeIn.bEnabled);
    boolean("initiative.enabled", settings.initiative.bEnabled);
    integer("initiative.maxPerHour", settings.initiative.maxUtterancesPerHour);
    integer("resources.usageSampleSeconds", settings.resources.usageSampleSeconds);
    boolean("conversation.archiveEnabled", settings.conversation.bArchiveEnabled);

    const auto temperature = values.find("llm.temperature");
    double parsedTemperature = 0.0;
    if (temperature != values.end() && ParseNumber(temperature->second, parsedTemperature))
    {
        settings.llm.temperature = static_cast<float>(parsedTemperature);
    }
    const auto profile = values.find("activeProfile");
    if (profile != values.end() && !profile->second.empty())
    {
        settings.activeProfile = profile->second;
    }
}

std::string PreferenceStore::Describe() const
{
    const std::map<std::string, std::string> values = Load();
    std::ostringstream stream;
    stream << "Saved preferences persist in " << storePath.string()
        << ". They cover comfort and pacing only; what Revia is permitted to do lives in "
           "capabilities.json and is changed in the Permissions tab.";
    for (const PreferenceKey& key : Writable())
    {
        const auto found = values.find(key.name);
        stream << "\n  " << key.name << "  ["
            << (found == values.end() ? std::string("default") : found->second) << "]  "
            << key.description;
    }
    return stream.str();
}

} // namespace revia::core
