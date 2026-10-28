#include "Core/appSettings.h"
#include "Core/preferenceStore.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace revia::core
{

namespace
{

// UI controls and foreground commands can save through different store instances.
// Keep the complete read-modify-write operation together without changing store moves.
std::mutex preferenceWriteMutex;

constexpr int ReplaceAttempts = 20;

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
        return consumed == value.size() && std::isfinite(out);
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

bool IsVoiceDevice(const std::string& value)
{
    return value == "auto" || value == "auto-secondary" || Lower(value) == "cpu" ||
        (value.size() > 4 && value.rfind("CUDA", 0) == 0 &&
            std::all_of(value.begin() + 4, value.end(),
                [](const unsigned char character) { return std::isdigit(character); }));
}

bool IsValidValue(const PreferenceKey& key, const std::string& value)
{
    switch (key.type)
    {
        case PreferenceType::Boolean:
        {
            bool parsed = false;
            return ParseBoolean(value, parsed);
        }
        case PreferenceType::Integer:
        case PreferenceType::Decimal:
        {
            double parsed = 0.0;
            return ParseNumber(value, parsed) && parsed >= key.minimum && parsed <= key.maximum &&
                (key.type != PreferenceType::Integer || std::trunc(parsed) == parsed);
        }
        case PreferenceType::Text:
            return !value.empty() &&
                (key.allowed.empty() || std::find(key.allowed.begin(), key.allowed.end(), value) != key.allowed.end()) &&
                (key.name != "resources.voiceDevice" || IsVoiceDevice(value));
    }
    return false;
}

// The settings field each writable preference controls, so one preference can be copied
// between two settings values without touching anything else.
template <auto Section, auto Member>
void CopyMember(appSettings& destination, const appSettings& source)
{
    (destination.*Section).*Member = (source.*Section).*Member;
}

template <auto Section, auto Member>
bool ReadFlag(const appSettings& settings)
{
    return (settings.*Section).*Member;
}

struct PreferenceField
{
    std::string_view name;
    void (*copy)(appSettings&, const appSettings&);
    // Boolean preferences only.
    bool (*flag)(const appSettings&);
};

template <auto Section, auto Member>
constexpr PreferenceField Flag(const std::string_view name)
{
    return {name, &CopyMember<Section, Member>, &ReadFlag<Section, Member>};
}

template <auto Section, auto Member>
constexpr PreferenceField Value(const std::string_view name)
{
    return {name, &CopyMember<Section, Member>, nullptr};
}

const std::array PreferenceFields = {
    Flag<&appSettings::speech, &speechSettings::bEnabled>("speech.enabled"),
    Value<&appSettings::speech, &speechSettings::volume>("speech.volume"),
    Value<&appSettings::speech, &speechSettings::rate>("speech.rate"),
    Flag<&appSettings::speech, &speechSettings::bSpeakGreeting>("speech.speakGreeting"),
    Flag<&appSettings::speechRecognition, &speechRecognitionSettings::bEnabled>("speechRecognition.enabled"),
    Flag<&appSettings::speechRecognition, &speechRecognitionSettings::bHandsFree>("speechRecognition.handsFree"),
    Flag<&appSettings::speechRecognition, &speechRecognitionSettings::bRequireWakeWord>("speechRecognition.requireWakeWord"),
    Flag<&appSettings::bargeIn, &bargeInSettings::bEnabled>("bargeIn.enabled"),
    Flag<&appSettings::initiative, &initiativeSettings::bEnabled>("initiative.enabled"),
    Value<&appSettings::initiative, &initiativeSettings::mode>("initiative.mode"),
    Flag<&appSettings::initiative, &initiativeSettings::bCuriosityEnabled>("initiative.curiosityEnabled"),
    Flag<&appSettings::initiative, &initiativeSettings::bSpontaneousSpeechEnabled>("initiative.spontaneousSpeechEnabled"),
    Flag<&appSettings::initiative, &initiativeSettings::bSpeakWhenUserAway>("initiative.speakWhenUserAway"),
    Flag<&appSettings::initiative, &initiativeSettings::bAutonomousLearningEnabled>("initiative.autonomousLearningEnabled"),
    Flag<&appSettings::initiative, &initiativeSettings::bSuppressWhenFullScreen>("initiative.suppressWhenFullScreen"),
    Value<&appSettings::initiative, &initiativeSettings::maxUtterancesPerHour>("initiative.maxPerHour"),
    Flag<&appSettings::presence, &presenceSettings::bAvatarBridgeEnabled>("presence.avatarBridgeEnabled"),
    Flag<&appSettings::presence, &presenceSettings::bExternalAdaptersEnabled>("presence.externalAdaptersEnabled"),
    Flag<&appSettings::presence, &presenceSettings::bEnabled>("presence.enabled"),
    Flag<&appSettings::presence, &presenceSettings::bRequireAddressedStreamMessages>("presence.requireAddressedStreamMessages"),
    Flag<&appSettings::presence, &presenceSettings::bSpeakStreamReplies>("presence.speakStreamReplies"),
    Value<&appSettings::llm, &llmSettings::temperature>("llm.temperature"),
    Value<&appSettings::resources, &resourceSettings::usageSampleSeconds>("resources.usageSampleSeconds"),
    Value<&appSettings::resources, &resourceSettings::voice>("resources.voiceDevice"),
    Flag<&appSettings::conversation, &conversationSettings::bArchiveEnabled>("conversation.archiveEnabled"),
    Value<&appSettings::conversation, &conversationSettings::delegation>("conversation.delegation"),
    Flag<&appSettings::conversation, &conversationSettings::bSelfInquiryEnabled>("conversation.selfInquiryEnabled"),
    Flag<&appSettings::conversation, &conversationSettings::bIterativeInvestigationEnabled>("conversation.iterativeInvestigationEnabled"),
    Flag<&appSettings::performance, &performanceSettings::bEnabled>("performance.enabled"),
    Flag<&appSettings::performance, &performanceSettings::bInterruptSongToSpeak>("performance.interruptSongToSpeak"),
    Flag<&appSettings::responseFilter, &responseFilterSettings::bAiReviewEnabled>("responseFilter.aiReviewEnabled"),
    PreferenceField{"activeProfile", [](appSettings& destination, const appSettings& source)
        { destination.activeProfile = source.activeProfile; }, nullptr},
};

const PreferenceField* FindField(const std::string_view name)
{
    const std::string wanted = Lower(std::string(name));
    const auto found = std::find_if(PreferenceFields.begin(), PreferenceFields.end(),
        [&wanted](const PreferenceField& field) { return Lower(std::string(field.name)) == wanted; });
    return found == PreferenceFields.end() ? nullptr : &*found;
}

} // namespace

bool CopySettingField(appSettings& destination, const appSettings& source, const std::string_view name)
{
    const PreferenceField* field = FindField(name);
    if (field == nullptr)
    {
        return false;
    }
    field->copy(destination, source);
    return true;
}

std::optional<bool> BooleanPreference(const appSettings& settings, const std::string_view name)
{
    const PreferenceField* field = FindField(name);
    if (field == nullptr || field->flag == nullptr)
    {
        return std::nullopt;
    }
    return field->flag(settings);
}

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
        {"speechRecognition.handsFree", PreferenceType::Boolean,
            "Use local VAD to listen for complete spoken thoughts without a button.", 0, 0, {}},
        {"speechRecognition.requireWakeWord", PreferenceType::Boolean,
            "In hands-free listening, answer only speech that says one of the wake words.", 0, 0, {}},
        {"bargeIn.enabled", PreferenceType::Boolean,
            "Let speaking over Revia interrupt her.", 0, 0, {}},
        {"initiative.enabled", PreferenceType::Boolean,
            "Allow Revia to start a conversation.", 0, 0, {}},
        {"initiative.mode", PreferenceType::Text,
            "Optional conversation pacing: normal, talkative, or focus.", 0, 0, {"normal", "talkative", "focus"}},
        {"initiative.curiosityEnabled", PreferenceType::Boolean,
            "Let Revia form evidence-based topics from conversation and affect.", 0, 0, {}},
        {"initiative.spontaneousSpeechEnabled", PreferenceType::Boolean,
            "Let an admitted curiosity become a spontaneous line.", 0, 0, {}},
        {"initiative.speakWhenUserAway", PreferenceType::Boolean,
            "Let Revia speak when Windows reports that the user is away.", 0, 0, {}},
        {"initiative.autonomousLearningEnabled", PreferenceType::Boolean,
            "Keep bounded, sourced findings from approved autonomous research.", 0, 0, {}},
        {"initiative.suppressWhenFullScreen", PreferenceType::Boolean,
            "Stay quiet while a full-screen application is in front.", 0, 0, {}},
        {"initiative.maxPerHour", PreferenceType::Integer,
            "Ceiling on unprompted openings per hour.", 0, 60, {}},
        {"presence.avatarBridgeEnabled", PreferenceType::Boolean,
            "Publish isolated avatar state and animation events.", 0, 0, {}},
        {"presence.externalAdaptersEnabled", PreferenceType::Boolean,
            "Accept conversation-only events from the bounded local adapter inbox.", 0, 0, {}},
        {"presence.enabled", PreferenceType::Boolean,
            "Run the avatar and streaming features.", 0, 0, {}},
        {"presence.requireAddressedStreamMessages", PreferenceType::Boolean,
            "Reply to a stream viewer only when the message is addressed to Revia.", 0, 0, {}},
        {"presence.speakStreamReplies", PreferenceType::Boolean,
            "Read replies to stream viewers aloud.", 0, 0, {}},
        {"llm.temperature", PreferenceType::Decimal,
            "Sampling temperature for replies.", 0.0, 2.0, {}},
        {"resources.usageSampleSeconds", PreferenceType::Integer,
            "How often live resource usage is sampled; 0 turns it off.", 0, 3600, {}},
        {"resources.voiceDevice", PreferenceType::Text,
            "Voice generation device: auto-secondary, cpu, or CUDA followed by an index.",
            0, 0, {}},
        {"conversation.archiveEnabled", PreferenceType::Boolean,
            "Keep a durable, searchable record of conversations.", 0, 0, {}},
        {"conversation.selfInquiryEnabled", PreferenceType::Boolean,
            "Think a hard question through before answering it.", 0, 0, {}},
        {"conversation.iterativeInvestigationEnabled", PreferenceType::Boolean,
            "Check what is still open after thinking a question through, before answering.", 0, 0, {}},
        {"performance.enabled", PreferenceType::Boolean,
            "Sing songs from the song library.", 0, 0, {}},
        {"performance.interruptSongToSpeak", PreferenceType::Boolean,
            "Stop singing to answer.", 0, 0, {}},
        // Pacing, not permission: it decides whether she may reach for her workers, never
        // what they may touch, which stays read-only inside the approved folders.
        {"conversation.delegation", PreferenceType::Text,
            "When Revia may hand an investigation to her agents from chat: off, on_request or auto.",
            0, 0, {"off", "on_request", "auto"}},
        {"responseFilter.aiReviewEnabled", PreferenceType::Boolean,
            "Run the AI response review after the always-on hard filter.", 0, 0, {}},
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
    // This exact mode is pacing only; other mode-like keys remain authority settings.
    if (lowered == "initiative.mode")
        return false;
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
        const PreferenceKey* definition = Find(key);
        if (definition != nullptr && value.is_string() &&
            IsValidValue(*definition, value.get<std::string>()))
        {
            values.emplace(definition->name, value.get<std::string>());
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
    // Windows refuses to replace a file another thread is reading (the Settings dialog reads
    // this one from the window's thread), so a refused move is retried briefly.
    for (int attempt = 0; attempt < ReplaceAttempts; ++attempt)
    {
        error.clear();
        std::filesystem::rename(temporary, storePath, error);
        if (!error)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::filesystem::remove(temporary, error);
    return false;
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
                if (std::trunc(parsed) != parsed)
                {
                    result.message = key->name + " takes a whole number.";
                    return result;
                }
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
            if (key->name == "resources.voiceDevice")
            {
                const bool automatic = value == "auto" || value == "auto-secondary";
                const bool cpu = Lower(value) == "cpu";
                if (!IsVoiceDevice(value))
                {
                    result.message = key->name +
                        " takes auto-secondary, cpu, or an exact device such as CUDA0.";
                    return result;
                }
                stored = automatic ? "auto-secondary" : (cpu ? "cpu" : value);
                break;
            }
            stored = value;
            break;
        }
    }

    std::lock_guard writeLock(preferenceWriteMutex);
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
    std::lock_guard writeLock(preferenceWriteMutex);
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
    boolean("speechRecognition.handsFree", settings.speechRecognition.bHandsFree);
    boolean("speechRecognition.requireWakeWord", settings.speechRecognition.bRequireWakeWord);
    boolean("bargeIn.enabled", settings.bargeIn.bEnabled);
    boolean("initiative.enabled", settings.initiative.bEnabled);
    const auto initiativeMode = values.find("initiative.mode");
    if (initiativeMode != values.end())
        settings.initiative.mode = initiativeMode->second;
    boolean("initiative.curiosityEnabled", settings.initiative.bCuriosityEnabled);
    boolean("initiative.spontaneousSpeechEnabled",
        settings.initiative.bSpontaneousSpeechEnabled);
    boolean("initiative.speakWhenUserAway", settings.initiative.bSpeakWhenUserAway);
    boolean("initiative.autonomousLearningEnabled",
        settings.initiative.bAutonomousLearningEnabled);
    integer("initiative.maxPerHour", settings.initiative.maxUtterancesPerHour);
    boolean("initiative.suppressWhenFullScreen", settings.initiative.bSuppressWhenFullScreen);
    boolean("presence.avatarBridgeEnabled", settings.presence.bAvatarBridgeEnabled);
    boolean("presence.externalAdaptersEnabled", settings.presence.bExternalAdaptersEnabled);
    boolean("presence.enabled", settings.presence.bEnabled);
    boolean("presence.requireAddressedStreamMessages", settings.presence.bRequireAddressedStreamMessages);
    boolean("presence.speakStreamReplies", settings.presence.bSpeakStreamReplies);
    integer("resources.usageSampleSeconds", settings.resources.usageSampleSeconds);
    boolean("conversation.archiveEnabled", settings.conversation.bArchiveEnabled);
    boolean("conversation.selfInquiryEnabled", settings.conversation.bSelfInquiryEnabled);
    boolean("conversation.iterativeInvestigationEnabled", settings.conversation.bIterativeInvestigationEnabled);
    boolean("performance.enabled", settings.performance.bEnabled);
    boolean("performance.interruptSongToSpeak", settings.performance.bInterruptSongToSpeak);
    boolean("responseFilter.aiReviewEnabled", settings.responseFilter.bAiReviewEnabled);

    const auto voiceDevice = values.find("resources.voiceDevice");
    if (voiceDevice != values.end() && !voiceDevice->second.empty())
    {
        settings.resources.voice = voiceDevice->second == "auto"
            ? "auto-secondary"
            : voiceDevice->second;
    }

    const auto temperature = values.find("llm.temperature");
    double parsedTemperature = 0.0;
    if (temperature != values.end() && ParseNumber(temperature->second, parsedTemperature))
    {
        settings.llm.temperature = static_cast<float>(parsedTemperature);
    }
    const auto delegation = values.find("conversation.delegation");
    if (delegation != values.end() && !delegation->second.empty())
    {
        settings.conversation.delegation = delegation->second;
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
