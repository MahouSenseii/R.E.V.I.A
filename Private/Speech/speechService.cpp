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
        if (!result.workerId.empty())
        {
            resource += " / " + result.workerId;
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
            case runtime::AffectState::Pleased:
            case runtime::AffectState::Excited:
            case runtime::AffectState::Playful: return 1;
            case runtime::AffectState::Concerned:
            case runtime::AffectState::Lonely:
            case runtime::AffectState::Bored:
            case runtime::AffectState::Sulky:
            case runtime::AffectState::Sad:
            case runtime::AffectState::Melancholy:
            case runtime::AffectState::Confused: return -1;
            case runtime::AffectState::Angry: return 1;
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
        qwenPool.Configure(settings);
        const std::string assigned = presetStore.AssignedPresetId(activeProfile);
        activePreset = assigned.empty() ? std::nullopt : presetStore.Find(assigned);
        eventHandler = std::move(handler);
        queue.clear();
        playbackOrder.clear();
        prepared.clear();
        generatingCount = 0;
        bufferedAudioBytes = 0;
    }
    enabled.store(settings.bEnabled);
    ready.store(false);
    generation.fetch_add(1);
    const std::size_t generatorCount = std::max<std::size_t>(
        1, std::min<std::size_t>(qwenPool.WorkerCount(),
            static_cast<std::size_t>(settings.qwenPrefetchFragments)));
    generationWorkers.clear();
    generationWorkers.reserve(generatorCount);
    for (std::size_t index = 0; index < generatorCount; ++index)
    {
        generationWorkers.emplace_back(
            [this](const std::stop_token stopToken) { Generate(stopToken); });
    }
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
    VoiceOperationResult result = qwenPool.PrepareCloneModel();
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
    VoiceOperationResult result = qwenPool.DesignVoice(
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
    VoiceOperationResult result = qwenPool.Synthesize(text, *preset, output.string());
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
            const std::uint64_t dropped = queue.front().sequence;
            queue.pop_front();
            playbackOrder.erase(std::remove(
                playbackOrder.begin(), playbackOrder.end(), dropped), playbackOrder.end());
        }
        Utterance utterance;
        utterance.text = std::move(text);
        utterance.affect = affect;
        utterance.generation = generation.load();
        utterance.utteranceId = utteranceId;
        utterance.sequence = nextSequence.fetch_add(1);
        if (configuration.backend != "WindowsSapi") utterance.preset = activePreset;
        playbackOrder.push_back(utterance.sequence);
        queue.push_back(std::move(utterance));
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
        playbackOrder.clear();
        for (const auto& [sequence, item] : prepared)
        {
            (void)sequence;
            if (!item.audioPath.empty())
            {
                std::error_code error;
                std::filesystem::remove(item.audioPath, error);
            }
        }
        prepared.clear();
        bufferedAudioBytes = 0;
    }
    condition.notify_all();
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
        playbackOrder.clear();
        for (const auto& [sequence, item] : prepared)
        {
            (void)sequence;
            std::error_code error;
            if (!item.audioPath.empty()) std::filesystem::remove(item.audioPath, error);
        }
        prepared.clear();
        bufferedAudioBytes = 0;
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

void SpeechService::CancelVoiceOperationsForShutdown()
{
    qwenPool.CancelActiveRequests();
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
    for (std::jthread& generator : generationWorkers)
    {
        generator.request_stop();
    }
    qwenPool.RequestShutdown();
    condition.notify_all();
    for (std::jthread& generator : generationWorkers)
    {
        if (generator.joinable()) generator.join();
    }
    generationWorkers.clear();
    ready.store(false);
    qwenPool.Shutdown();
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
        PreparedUtterance preparedUtterance;
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, stopToken, [this]
            {
                return !playbackOrder.empty() &&
                    prepared.contains(playbackOrder.front());
            });
            if (stopToken.stop_requested())
            {
                break;
            }
            if (playbackOrder.empty() || !enabled.load())
            {
                continue;
            }
            const std::uint64_t sequence = playbackOrder.front();
            playbackOrder.pop_front();
            auto found = prepared.find(sequence);
            if (found == prepared.end()) continue;
            preparedUtterance = std::move(found->second);
            prepared.erase(found);
            bufferedAudioBytes = preparedUtterance.bufferedBytes >= bufferedAudioBytes
                ? 0
                : bufferedAudioBytes - preparedUtterance.bufferedBytes;
        }
        condition.notify_all();
        const Utterance& utterance = preparedUtterance.utterance;
        if (utterance.generation != generation.load())
        {
            std::error_code error;
            if (!preparedUtterance.audioPath.empty())
                std::filesystem::remove(preparedUtterance.audioPath, error);
            continue;
        }
        activeUtteranceId.store(utterance.utteranceId);
        // Cleared however this iteration ends, so an event published between utterances is
        // never mislabelled as belonging to the last reply.
        struct ActiveGuard
        {
            std::atomic<std::uint64_t>& id;
            ~ActiveGuard() { id.store(0); }
        } activeGuard{activeUtteranceId};

        if (preparedUtterance.qwenAttempted && preparedUtterance.result.succeeded &&
            PlayPreparedQwen(preparedUtterance))
        {
            continue;
        }
        if (preparedUtterance.qwenAttempted && !preparedUtterance.result.succeeded)
        {
            Notify({"Fallback", preparedUtterance.result.message +
                " Using Windows voice for this sentence.",
                preparedUtterance.result.elapsedMilliseconds, 0, utterance.utteranceId,
                ActualQwenResource(preparedUtterance.result)});
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

void SpeechService::Generate(const std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        Utterance utterance;
        int depth = 0;
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, stopToken, [this]
            {
                const std::uintmax_t maximumBytes =
                    static_cast<std::uintmax_t>(configuration.qwenMaxBufferedAudioMiB) *
                    1024U * 1024U;
                return !queue.empty() &&
                    generatingCount + prepared.size() <
                        static_cast<std::size_t>(configuration.qwenPrefetchFragments) &&
                    bufferedAudioBytes < maximumBytes;
            });
            if (stopToken.stop_requested()) return;
            if (queue.empty()) continue;
            utterance = std::move(queue.front());
            queue.pop_front();
            ++generatingCount;
            depth = static_cast<int>(queue.size());
        }

        PreparedUtterance item;
        item.utterance = utterance;
        if (utterance.preset.has_value())
        {
            item.qwenAttempted = true;
            std::error_code error;
            item.audioPath = std::filesystem::absolute(
                presetStore.Root() / "Playback" /
                    (utterance.preset->id + "-" + std::to_string(utterance.generation) +
                     "-" + std::to_string(utterance.sequence) + ".wav"), error).lexically_normal();
            if (error)
            {
                item.result = {false, "The Qwen playback path could not be resolved.", {}, -1.0};
            }
            else
            {
                std::filesystem::create_directories(item.audioPath.parent_path(), error);
                Notify({"Generating", "Synthesizing a sentence ahead with Qwen3-TTS.",
                    -1.0, depth, utterance.utteranceId,
                    PlannedQwenResource(configuration)});
                item.result = qwenPool.Synthesize(
                    utterance.text, *utterance.preset, item.audioPath.string());
                if (item.result.succeeded)
                {
                    item.bufferedBytes = std::filesystem::file_size(item.audioPath, error);
                    if (error) item.bufferedBytes = 0;
                }
            }
        }

        const bool stale = utterance.generation != generation.load() || !enabled.load();
        const VoiceOperationResult completedResult = item.result;
        {
            std::lock_guard lock(mutex);
            if (generatingCount > 0) --generatingCount;
            if (!stale)
            {
                bufferedAudioBytes += item.bufferedBytes;
                prepared.emplace(utterance.sequence, std::move(item));
            }
        }
        if (stale)
        {
            std::error_code error;
            if (!item.audioPath.empty()) std::filesystem::remove(item.audioPath, error);
        }
        else if (utterance.preset.has_value() && completedResult.succeeded)
        {
            Notify({"Generated", "Qwen3-TTS sentence audio is ready for ordered playback.",
                completedResult.elapsedMilliseconds, depth, utterance.utteranceId,
                ActualQwenResource(completedResult)});
        }
        condition.notify_all();
    }
}

bool SpeechService::PlayPreparedQwen(const PreparedUtterance& preparedUtterance)
{
#ifndef _WIN32
    (void)preparedUtterance;
    return false;
#else
    const Utterance& utterance = preparedUtterance.utterance;
    const VoiceOperationResult& generated = preparedUtterance.result;
    const std::filesystem::path& output = preparedUtterance.audioPath;
    std::error_code error;
    const auto startedAt = std::chrono::steady_clock::now();
    if (generation.load() != utterance.generation || !enabled.load())
    {
        std::filesystem::remove(output, error);
        Notify({"Stopped", "Generated speech was cancelled before playback.",
            ElapsedMilliseconds(startedAt), 0, 0, ActualQwenResource(generated)});
        return true;
    }
    SpeechEvent playing{"Speaking", "Playing the profile's Qwen3-TTS voice."};
    playing.utteranceId = utterance.utteranceId;
    playing.device = ActualQwenResource(generated);
    Notify(std::move(playing));
    if (!PlaySoundW(output.wstring().c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
    {
        Notify({"Fallback", "Windows could not play the Qwen3-TTS WAV; using SAPI.",
            -1.0, 0, utterance.utteranceId, WindowsSapiResource});
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
        ElapsedMilliseconds(startedAt), 0, utterance.utteranceId,
        ActualQwenResource(generated)});
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
