#include "Speech/speechService.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <sapi.h>
#endif

namespace revia::speech
{

namespace
{
    double ElapsedMilliseconds(const std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }

    constexpr const char* WindowsSapiResource = "CPU / Windows SAPI";

    std::string PlannedQwenResource(const speechSettings& settings)
    {
        return settings.qwenDevice == "cpu" ? "CPU / Qwen3-TTS" : settings.qwenDevice;
    }

    std::string ActualQwenResource(const VoiceOperationResult& result)
    {
        std::string resource = result.device.empty() ? "Qwen3-TTS" : result.device;
        if (!result.deviceName.empty())
        {
            resource += " / " + result.deviceName;
        }
        if (!result.dtype.empty())
        {
            resource += " / " + result.dtype;
        }
        return resource;
    }

#ifdef _WIN32
    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
        {
            return {};
        }
        const int length = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (length <= 0)
        {
            return {};
        }
        std::wstring output(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            output.data(), length);
        return output;
    }

    long AffectRateAdjustment(const runtime::AffectState state)
    {
        switch (state)
        {
            case runtime::AffectState::Curious:
            case runtime::AffectState::Pleased: return 1;
            case runtime::AffectState::Concerned:
            case runtime::AffectState::Confused: return -1;
            default: return 0;
        }
    }

    double WavDurationMilliseconds(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return 0.0;
        }
        char header[44]{};
        file.read(header, sizeof(header));
        if (file.gcount() != sizeof(header) || std::string_view(header, 4) != "RIFF" ||
            std::string_view(header + 8, 4) != "WAVE")
        {
            return 0.0;
        }
        const auto read32 = [&](const int offset)
        {
            const auto* bytes = reinterpret_cast<const unsigned char*>(header + offset);
            return static_cast<std::uint32_t>(bytes[0]) |
                (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                (static_cast<std::uint32_t>(bytes[3]) << 24U);
        };
        const std::uint32_t byteRate = read32(28);
        const std::uint32_t dataSize = read32(40);
        return byteRate == 0 ? 0.0 :
            static_cast<double>(dataSize) * 1000.0 / static_cast<double>(byteRate);
    }
#endif

    std::string Slugify(const std::string& value)
    {
        std::string output;
        bool separator = false;
        for (const unsigned char character : value)
        {
            if (std::isalnum(character))
            {
                output.push_back(static_cast<char>(std::tolower(character)));
                separator = false;
            }
            else if (!output.empty() && !separator)
            {
                output.push_back('-');
                separator = true;
            }
        }
        while (!output.empty() && output.back() == '-')
        {
            output.pop_back();
        }
        return output.substr(0, 48);
    }

    std::string IsoTimestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &time);
#else
        gmtime_r(&time, &utc);
#endif
        std::ostringstream stream;
        stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        return stream.str();
    }
}

SpeechService::~SpeechService()
{
    Shutdown();
}

bool SpeechService::Start(const speechSettings& settings, EventHandler handler)
{
    Shutdown();
    {
        std::lock_guard lock(mutex);
        configuration = settings;
        presetStore.SetRoot(settings.voiceDataPath);
        qwenClient.Configure(settings);
        const std::string assigned = presetStore.AssignedPresetId(activeProfile);
        activePreset = assigned.empty() ? std::nullopt : presetStore.Find(assigned);
        eventHandler = std::move(handler);
        queue.clear();
    }
    enabled.store(settings.bEnabled);
    ready.store(false);
    generation.fetch_add(1);
    worker = std::jthread([this](const std::stop_token stopToken) { Run(stopToken); });
    return true;
}

void SpeechService::SetActiveProfile(std::string profileId)
{
    std::lock_guard lock(mutex);
    activeProfile = std::move(profileId);
    const std::string assigned = presetStore.AssignedPresetId(activeProfile);
    activePreset = assigned.empty() ? std::nullopt : presetStore.Find(assigned);
}

VoiceStudioSnapshot SpeechService::VoiceStudio() const
{
    VoiceStudioSnapshot snapshot;
    {
        std::lock_guard lock(mutex);
        snapshot.activeProfile = activeProfile;
    }
    snapshot.presets = presetStore.List();
    snapshot.assignedPresetId = presetStore.AssignedPresetId(snapshot.activeProfile);

    std::error_code error;
    const std::filesystem::path profileRoot = std::filesystem::absolute("Config/Profiles", error);
    if (!error && std::filesystem::is_directory(profileRoot, error))
    {
        for (const auto& entry : std::filesystem::directory_iterator(profileRoot, error))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".json")
            {
                snapshot.profiles.push_back(entry.path().stem().string());
            }
        }
    }
    if (snapshot.profiles.empty() && !snapshot.activeProfile.empty())
    {
        snapshot.profiles.push_back(snapshot.activeProfile);
    }
    std::sort(snapshot.profiles.begin(), snapshot.profiles.end());
    for (const std::string& profileId : snapshot.profiles)
    {
        const std::string assigned = presetStore.AssignedPresetId(profileId);
        if (!assigned.empty())
        {
            snapshot.profileAssignments.emplace(profileId, assigned);
        }
    }
    return snapshot;
}

bool SpeechService::HasActiveQwenVoice() const
{
    std::lock_guard lock(mutex);
    return configuration.backend != "WindowsSapi" && activePreset.has_value();
}

VoiceOperationResult SpeechService::PrepareActiveVoice()
{
    if (!HasActiveQwenVoice())
    {
        return {true, "No Qwen3-TTS profile voice is assigned; Windows SAPI is ready.", {}, 0.0};
    }
    Notify({"Loading", "Loading the assigned Qwen3-TTS voice on the selected device.",
        -1.0, 0, 0, PlannedQwenResource(configuration)});
    VoiceOperationResult result = qwenClient.PrepareCloneModel();
    SpeechEvent prepared{result.succeeded ? "Ready" : "Fallback",
        result.succeeded
            ? result.message
            : result.message + " Windows SAPI remains available.",
        result.elapsedMilliseconds};
    prepared.device = ActualQwenResource(result);
    Notify(std::move(prepared));
    return result;
}

VoiceOperationResult SpeechService::CreateVoicePreset(
    const std::string& name,
    const std::string& description,
    const std::string& referenceText,
    const std::string& language)
{
    const std::string id = Slugify(name);
    if (!VoicePresetStore::IsSafeId(id) || description.empty() || referenceText.empty())
    {
        return {false, "A preset name, voice description, and reference line are required.", {}, -1.0};
    }
    const std::filesystem::path directory = presetStore.Root() / id;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        return {false, "Could not create the voice preset directory: " + error.message(), {}, -1.0};
    }
    const std::filesystem::path referencePath =
        std::filesystem::absolute(directory / "reference.wav", error).lexically_normal();
    if (error)
    {
        return {false, "Could not resolve the voice reference path.", {}, -1.0};
    }
    Notify({"Designing", "Creating a reusable Qwen3-TTS voice reference. First use downloads the model.",
        -1.0, 0, 0, PlannedQwenResource(configuration)});
    VoiceOperationResult result = qwenClient.DesignVoice(
        referenceText, description, language.empty() ? "English" : language,
        referencePath.string());
    if (!result.succeeded)
    {
        Notify({"Error", result.message, result.elapsedMilliseconds, 0, 0,
            ActualQwenResource(result)});
        return result;
    }
    VoicePreset preset;
    preset.id = id;
    preset.name = name;
    preset.description = description;
    preset.language = language.empty() ? "English" : language;
    preset.referenceText = referenceText;
    preset.referenceAudioPath = (directory / "reference.wav").lexically_normal().string();
    preset.createdAt = IsoTimestamp();
    std::string saveError;
    if (!presetStore.Save(preset, saveError))
    {
        return {false, saveError, {}, -1.0};
    }
    result.message = "Created voice preset '" + name + "'.";
    result.outputPath = referencePath.string();
    Notify({"Ready", result.message, result.elapsedMilliseconds, 0, 0,
        ActualQwenResource(result)});
    return result;
}

VoiceOperationResult SpeechService::PreviewVoice(
    const std::string& presetId,
    const std::string& text)
{
    const auto preset = presetStore.Find(presetId);
    if (!preset)
    {
        return {false, "The selected voice preset does not exist.", {}, -1.0};
    }
    if (text.empty())
    {
        return {false, "Enter a preview line first.", {}, -1.0};
    }
    std::error_code error;
    const std::filesystem::path output = std::filesystem::absolute(
        presetStore.Root() / "Preview" / (presetId + ".wav"), error).lexically_normal();
    if (error)
    {
        return {false, "Could not resolve the preview path.", {}, -1.0};
    }
    std::filesystem::create_directories(output.parent_path(), error);
    Notify({"Generating", "Generating a Qwen3-TTS voice preview.", -1.0, 0, 0,
        PlannedQwenResource(configuration)});
    VoiceOperationResult result = qwenClient.Synthesize(text, *preset, output.string());
#ifdef _WIN32
    if (result.succeeded)
    {
        PlaySoundW(output.wstring().c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    }
#endif
    Notify({result.succeeded ? "Ready" : "Error", result.message,
        result.elapsedMilliseconds, 0, 0, ActualQwenResource(result)});
    return result;
}

VoiceOperationResult SpeechService::AssignVoice(
    const std::string& profileId,
    const std::string& presetId)
{
    std::string error;
    if (!presetStore.Assign(profileId, presetId, error))
    {
        return {false, error, {}, -1.0};
    }
    bool assignedActiveProfile = false;
    {
        std::lock_guard lock(mutex);
        assignedActiveProfile = profileId == activeProfile;
        if (profileId == activeProfile)
        {
            activePreset = presetId.empty() ? std::nullopt : presetStore.Find(presetId);
        }
    }
    std::string message = presetId.empty()
        ? "The profile will use Windows voice fallback."
        : "Voice preset assigned to profile '" + profileId + "'.";
    if (!presetId.empty() && assignedActiveProfile)
    {
        message += " Restart Revia once so Auto mode can load the voice before llama.cpp "
            "and fit both models to the GPU.";
    }
    Notify({"Ready", message});
    return {true, message, {}, 0.0};
}

void SpeechService::SetEnabled(const bool value)
{
    enabled.store(value);
    if (!value)
    {
        StopSpeaking();
        Notify({"Disabled", "Voice output is off."});
    }
    else
    {
        condition.notify_all();
        Notify({ready.load() ? "Ready" : "Starting",
            ready.load() ? "Voice output is ready." : "Voice output is starting."});
    }
}

bool SpeechService::IsEnabled() const
{
    return enabled.load();
}

void SpeechService::Speak(
    std::string text,
    const runtime::AffectSnapshot affect,
    const std::uint64_t utteranceId)
{
    if (!enabled.load())
    {
        return;
    }
    text = NormalizeForSpeech(text, static_cast<std::size_t>(configuration.maxCharacters));
    if (text.empty())
    {
        return;
    }

    int depth = 0;
    {
        std::lock_guard lock(mutex);
        while (queue.size() >= static_cast<std::size_t>(configuration.maxQueuedUtterances))
        {
            queue.pop_front();
        }
        queue.push_back({std::move(text), affect, generation.load(), utteranceId});
        depth = static_cast<int>(queue.size());
    }
    Notify({"Queued", "Assistant reply queued for speech.", -1.0, depth, utteranceId});
    condition.notify_all();
}

void SpeechService::StopSpeaking()
{
    generation.fetch_add(1);
    {
        std::lock_guard lock(mutex);
        queue.clear();
    }
    condition.notify_all();
    qwenClient.CancelActiveRequest();
#ifdef _WIN32
    PlaySoundW(nullptr, nullptr, 0);
#endif
}

void SpeechService::ConfigureBargeIn(const bargeInSettings& settings, const int sampleRate)
{
    bargeInMonitor.Configure(settings, sampleRate);
}

void SpeechService::SetBargeInHandler(std::function<void()> handler)
{
    std::lock_guard lock(mutex);
    bargeInHandler = std::move(handler);
}

void SpeechService::SetBargeInEnabled(const bool enabled)
{
    bargeInMonitor.SetEnabled(enabled);
    if (!enabled)
    {
        // Disarm whatever is listening right now, so turning this off takes effect in the
        // middle of the reply that prompted the user to turn it off.
        bargeInMonitor.End();
    }
}

bool SpeechService::IsBargeInEnabled() const
{
    return bargeInMonitor.IsEnabled();
}

void SpeechService::YieldToUser()
{
    generation.fetch_add(1);
    {
        std::lock_guard lock(mutex);
        queue.clear();
    }
    condition.notify_all();
#ifdef _WIN32
    PlaySoundW(nullptr, nullptr, 0);
#endif
}

void SpeechService::ArmBargeIn()
{
    bargeInMonitor.Begin([this]()
    {
        // Stop first, then tell anyone listening. The gap between the user speaking and
        // Revia going quiet is the whole quality of this feature.
        YieldToUser();
        Notify({"Interrupted", "You started speaking, so I stopped."});
        std::function<void()> handler;
        {
            std::lock_guard lock(mutex);
            handler = bargeInHandler;
        }
        if (handler)
        {
            handler();
        }
    });
}

void SpeechService::DisarmBargeIn()
{
    bargeInMonitor.End();
}

void SpeechService::Shutdown()
{
    enabled.store(false);
    StopSpeaking();
    if (worker.joinable())
    {
        worker.request_stop();
        condition.notify_all();
        worker.join();
    }
    ready.store(false);
    qwenClient.Shutdown();
}

std::string SpeechService::NormalizeForSpeech(
    const std::string& text,
    const std::size_t maxCharacters)
{
    std::istringstream input(text);
    std::ostringstream visible;
    std::string line;
    bool inCodeBlock = false;
    while (std::getline(input, line))
    {
        if (line.find("```") != std::string::npos)
        {
            inCodeBlock = !inCodeBlock;
            continue;
        }
        if (!inCodeBlock)
        {
            visible << line << ' ';
        }
    }

    const std::string withoutMarkdownLinks = std::regex_replace(
        visible.str(), std::regex(R"(\[([^\]]+)\]\([^)]+\))"), "$1");
    std::istringstream words(withoutMarkdownLinks);
    std::string token;
    std::string output;
    while (words >> token)
    {
        if (token.starts_with("http://") || token.starts_with("https://") ||
            token.starts_with("www."))
        {
            continue;
        }
        token.erase(std::remove_if(token.begin(), token.end(), [](const char character)
        {
            return character == '*' || character == '`' || character == '#' || character == '_';
        }), token.end());
        if (token.empty())
        {
            continue;
        }
        if (!output.empty())
        {
            output.push_back(' ');
        }
        output += token;
        if (output.size() >= maxCharacters)
        {
            output.resize(maxCharacters);
            const std::size_t lastSpace = output.find_last_of(' ');
            if (lastSpace != std::string::npos && lastSpace > maxCharacters / 2)
            {
                output.resize(lastSpace);
            }
            output += ".";
            break;
        }
    }
    return output;
}

void SpeechService::Run(const std::stop_token stopToken)
{
#ifdef _WIN32
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized))
    {
        Notify({"Error", "Windows speech could not initialize.", -1.0, 0, 0,
            WindowsSapiResource});
        return;
    }

    ISpVoice* voice = nullptr;
    const HRESULT created = CoCreateInstance(
        CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
        reinterpret_cast<void**>(&voice));
    if (FAILED(created) || voice == nullptr)
    {
        Notify({"Error", "No Windows SAPI voice is available.", -1.0, 0, 0,
            WindowsSapiResource});
        CoUninitialize();
        return;
    }

    voice->SetVolume(static_cast<USHORT>(configuration.volume));
    ready.store(true);
    Notify({enabled.load() ? "Ready" : "Disabled",
        enabled.load() ? "Windows SAPI voice is ready." : "Voice output is off.",
        -1.0, 0, 0, WindowsSapiResource});

    while (!stopToken.stop_requested())
    {
        Utterance utterance;
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, stopToken, [this]
            {
                return !queue.empty();
            });
            if (stopToken.stop_requested())
            {
                break;
            }
            if (queue.empty() || !enabled.load())
            {
                continue;
            }
            utterance = std::move(queue.front());
            queue.pop_front();
        }
        activeUtteranceId.store(utterance.utteranceId);
        // Cleared however this iteration ends, so an event published between utterances is
        // never mislabelled as belonging to the last reply.
        struct ActiveGuard
        {
            std::atomic<std::uint64_t>& id;
            ~ActiveGuard() { id.store(0); }
        } activeGuard{activeUtteranceId};

        std::optional<VoicePreset> qwenPreset;
        {
            std::lock_guard lock(mutex);
            if (configuration.backend != "WindowsSapi")
            {
                qwenPreset = activePreset;
            }
        }
        if (qwenPreset && SpeakWithQwen(utterance, *qwenPreset))
        {
            continue;
        }

        const std::uint64_t activeGeneration = utterance.generation;
        const std::wstring speechText = Utf8ToWide(utterance.text);
        if (speechText.empty())
        {
            Notify({"Error", "The reply could not be converted for speech.", -1.0, 0, 0,
                WindowsSapiResource});
            continue;
        }
        voice->SetRate(std::clamp<long>(
            static_cast<long>(configuration.rate) + AffectRateAdjustment(utterance.affect.state),
            -10L, 10L));
        const auto startedAt = std::chrono::steady_clock::now();
        Notify({"Speaking", "Reading the assistant reply aloud.", -1.0, 0, 0,
            WindowsSapiResource});
        const HRESULT spoke = voice->Speak(
            speechText.c_str(), static_cast<DWORD>(SPF_ASYNC | SPF_IS_NOT_XML), nullptr);
        if (FAILED(spoke))
        {
            Notify({"Error", "Windows SAPI could not start the utterance.", -1.0, 0, 0,
                WindowsSapiResource});
            continue;
        }
        ArmBargeIn();

        bool cancelled = false;
        while (!stopToken.stop_requested())
        {
            if (generation.load() != activeGeneration || !enabled.load())
            {
                voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
                cancelled = true;
                break;
            }
            if (voice->WaitUntilDone(40) == S_OK)
            {
                break;
            }
        }
        const bool interrupted = bargeInMonitor.Triggered();
        DisarmBargeIn();
        if (stopToken.stop_requested())
        {
            voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
            break;
        }
        const double elapsed = ElapsedMilliseconds(startedAt);
        Notify({interrupted ? "Interrupted" : (cancelled ? "Stopped" : "Ready"),
            interrupted
                ? "You started speaking, so I stopped."
                : (cancelled ? "Speech was stopped." : "Speech completed."),
            elapsed, 0, 0, WindowsSapiResource});
    }

    voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
    voice->Release();
    ready.store(false);
    CoUninitialize();
#else
    (void)stopToken;
    Notify({"Unavailable", "Voice output is currently implemented for Windows."});
#endif
}

bool SpeechService::SpeakWithQwen(const Utterance& utterance, const VoicePreset& preset)
{
#ifndef _WIN32
    (void)utterance;
    (void)preset;
    return false;
#else
    std::error_code error;
    const std::filesystem::path output = std::filesystem::absolute(
        presetStore.Root() / "Playback" /
            (preset.id + "-" + std::to_string(utterance.generation) + ".wav"), error).lexically_normal();
    if (error)
    {
        return false;
    }
    std::filesystem::create_directories(output.parent_path(), error);
    const auto startedAt = std::chrono::steady_clock::now();
    Notify({"Generating", "Synthesizing the assistant reply with Qwen3-TTS.",
        -1.0, 0, 0, PlannedQwenResource(configuration)});
    const VoiceOperationResult generated = qwenClient.Synthesize(
        utterance.text, preset, output.string());
    if (!generated.succeeded)
    {
        Notify({"Fallback", generated.message + " Using Windows voice for this reply.",
            generated.elapsedMilliseconds, 0, 0, ActualQwenResource(generated)});
        return false;
    }
    if (generation.load() != utterance.generation || !enabled.load())
    {
        std::filesystem::remove(output, error);
        Notify({"Stopped", "Generated speech was cancelled before playback.",
            ElapsedMilliseconds(startedAt), 0, 0, ActualQwenResource(generated)});
        return true;
    }
    SpeechEvent playing{"Speaking", "Playing the profile's Qwen3-TTS voice."};
    playing.device = ActualQwenResource(generated);
    Notify(std::move(playing));
    if (!PlaySoundW(output.wstring().c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
    {
        Notify({"Fallback", "Windows could not play the Qwen3-TTS WAV; using SAPI.",
            -1.0, 0, 0, WindowsSapiResource});
        return false;
    }
    ArmBargeIn();
    const double duration = std::max(250.0, WavDurationMilliseconds(output));
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(static_cast<long long>(duration + 150.0));
    bool cancelled = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (generation.load() != utterance.generation || !enabled.load())
        {
            PlaySoundW(nullptr, nullptr, 0);
            cancelled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    const bool interrupted = bargeInMonitor.Triggered();
    DisarmBargeIn();
    std::filesystem::remove(output, error);
    Notify({interrupted ? "Interrupted" : (cancelled ? "Stopped" : "Ready"),
        interrupted
            ? "You started speaking, so I stopped."
            : (cancelled ? "Speech was stopped." : "Qwen3-TTS speech completed."),
        ElapsedMilliseconds(startedAt), 0, 0, ActualQwenResource(generated)});
    return true;
#endif
}

void SpeechService::Notify(SpeechEvent event) const
{
    if (event.utteranceId == 0)
    {
        event.utteranceId = activeUtteranceId.load();
    }
    EventHandler handler;
    {
        std::lock_guard lock(mutex);
        handler = eventHandler;
    }
    if (handler)
    {
        handler(event);
    }
}

} // namespace revia::speech
