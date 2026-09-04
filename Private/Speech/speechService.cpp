#include "Speech/speechService.h"

#include "Speech/vocalization.h"

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
        char riff[12]{};
        file.read(riff, sizeof(riff));
        if (file.gcount() != sizeof(riff) || std::string_view(riff, 4) != "RIFF" ||
            std::string_view(riff + 8, 4) != "WAVE")
        {
            return 0.0;
        }
        const auto decode32 = [](const unsigned char* bytes)
        {
            return static_cast<std::uint32_t>(bytes[0]) |
                (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                (static_cast<std::uint32_t>(bytes[3]) << 24U);
        };
        std::uint32_t byteRate = 0;
        std::uint32_t dataSize = 0;
        while (file && (byteRate == 0 || dataSize == 0))
        {
            char chunkHeader[8]{};
            file.read(chunkHeader, sizeof(chunkHeader));
            if (file.gcount() != sizeof(chunkHeader)) break;
            const std::uint32_t chunkSize = decode32(
                reinterpret_cast<const unsigned char*>(chunkHeader + 4));
            if (std::string_view(chunkHeader, 4) == "fmt " && chunkSize >= 12)
            {
                std::vector<unsigned char> format(std::min<std::uint32_t>(chunkSize, 16));
                file.read(reinterpret_cast<char*>(format.data()),
                    static_cast<std::streamsize>(format.size()));
                if (format.size() >= 12 && file.gcount() >= 12)
                {
                    byteRate = decode32(format.data() + 8);
                }
                if (chunkSize > format.size())
                {
                    file.seekg(static_cast<std::streamoff>(chunkSize - format.size()),
                        std::ios::cur);
                }
            }
            else
            {
                if (std::string_view(chunkHeader, 4) == "data") dataSize = chunkSize;
                file.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
            }
            if ((chunkSize & 1U) != 0) file.seekg(1, std::ios::cur);
        }
        return byteRate == 0 ? 0.0 :
            static_cast<double>(dataSize) * 1000.0 / static_cast<double>(byteRate);
    }

    double WavDurationMilliseconds(const std::vector<std::uint8_t>& bytes)
    {
        if (bytes.size() < 44 ||
            std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) != "RIFF" ||
            std::string_view(reinterpret_cast<const char*>(bytes.data() + 8), 4) != "WAVE")
        {
            return 0.0;
        }
        const auto read32 = [&bytes](const std::size_t offset)
        {
            return static_cast<std::uint32_t>(bytes[offset]) |
                (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
                (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
                (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
        };
        std::uint32_t byteRate = 0;
        std::uint32_t dataSize = 0;
        std::size_t cursor = 12;
        while (cursor + 8 <= bytes.size() && (byteRate == 0 || dataSize == 0))
        {
            const std::uint32_t chunkSize = read32(cursor + 4);
            const std::size_t payload = cursor + 8;
            if (payload + static_cast<std::size_t>(chunkSize) > bytes.size()) break;
            const std::string_view id(
                reinterpret_cast<const char*>(bytes.data() + cursor), 4);
            if (id == "fmt " && chunkSize >= 12)
            {
                byteRate = read32(payload + 8);
            }
            else if (id == "data")
            {
                dataSize = chunkSize;
            }
            cursor = payload + static_cast<std::size_t>(chunkSize) + (chunkSize & 1U);
        }
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
        playbackOrder.Clear();
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
    std::optional<VoicePreset> preset;
    {
        std::lock_guard lock(mutex);
        preset = activePreset;
    }
    if (!preset)
    {
        return {true, "No Qwen3-TTS voice needs preparation.", {}, 0.0};
    }
    VoiceOperationResult result = qwenPool.PrepareVoice(*preset);
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

bool SpeechService::HasPendingSpeech() const
{
    std::lock_guard lock(mutex);
    return !playbackOrder.Empty() || activeUtteranceId.load() != 0;
}

std::size_t SpeechService::PreferredFragmentCharacters() const
{
    return configuration.bQwenParallelLongReplies
        ? static_cast<std::size_t>(std::max(48, configuration.qwenPhraseCharacters))
        : 0;
}

std::size_t SpeechService::FirstFragmentCharacters() const
{
    return configuration.bQwenParallelLongReplies
        ? static_cast<std::size_t>(std::max(
            16, configuration.qwenFirstPhraseCharacters))
        : 0;
}

void SpeechService::Speak(
    std::string text,
    const runtime::AffectSnapshot affect,
    const std::uint64_t utteranceId,
    const bool latencyCritical)
{
    if (!enabled.load())
    {
        return;
    }
    // Vocalization tags never reach any synthesiser now, on any backend.
    //
    // They used to be preserved for Qwen on the assumption that the model would
    // perform the cue rather than read it. Live listening settled that: it says the
    // word. "chuckles" spoken aloud in the middle of a sentence is worse than the
    // silence of dropping it, and dropping it is what the SAPI path already did.
    //
    // The tag survives in the visible reply, which is where it belongs -- the chat
    // bubble still shows *chuckles*. What it no longer does is become a word.
    //
    // Making the sound actually happen is a separate, larger piece of work: it needs
    // the parsed segments played in order against a clip bank, and the bank is empty
    // on this machine. Until that exists, silence is the correct behaviour rather
    // than narration.
    text = PrepareForSynthesis(text, configuration);
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
            playbackOrder.Remove(dropped);
        }
        Utterance utterance;
        utterance.text = std::move(text);
        utterance.affect = affect;
        utterance.generation = generation.load();
        utterance.utteranceId = utteranceId;
        utterance.sequence = nextSequence.fetch_add(1);
        utterance.latencyCritical = latencyCritical;
        utterance.queuedAt = std::chrono::steady_clock::now();
        if (configuration.backend != "WindowsSapi") utterance.preset = activePreset;
        playbackOrder.Reserve(utterance.sequence);
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
        playbackOrder.Clear();
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
        playbackOrder.Clear();
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

std::string SpeechService::PrepareForSynthesis(
    const std::string& text,
    const speechSettings& settings)
{
    // false, for every backend, deliberately and unconditionally. Qwen would
    // synthesise the word "chuckles"; SAPI would read it aloud; a backend nobody has
    // written yet has not earned the benefit of the doubt. Making the sound actually
    // happen belongs to the clip bank, not to a synthesiser being handed a stage
    // direction, and until a cue has a clip, silence beats narration.
    return NormalizeForSpeech(
        text,
        static_cast<std::size_t>(settings.maxCharacters),
        false);
}

std::string SpeechService::NormalizeForSpeech(
    const std::string& text,
    const std::size_t maxCharacters,
    const bool keepVocalizations)
{
    // A vocalization tag is the one asterisk pair that must reach the model intact.
    // Everything below strips '*' as markdown noise, and flattening "*laughs*" to
    // "laughs" is precisely the failure this feature exists to avoid: the voice reads
    // the word instead of making the sound.
    // All three bracket styles, because ParseVocalizations accepts all three and the
    // small local model writing these is not consistent about which it uses. This
    // recognised only the asterisk form, so "<sigh>" and "[laugh]" walked straight
    // past it and were spoken as words -- the same live defect, one bracket over.
    const auto vocalizationTag = [](const std::string& token) -> std::string
    {
        if (token.size() < 3)
        {
            return {};
        }
        char closingCharacter = '\0';
        switch (token.front())
        {
            case '*': closingCharacter = '*'; break;
            case '[': closingCharacter = ']'; break;
            case '<': closingCharacter = '>'; break;
            default: return {};
        }
        const std::size_t closing = token.find(closingCharacter, 1);
        if (closing == std::string::npos || closing == 1)
        {
            return {};
        }
        VocalizationKind kind{};
        if (!VocalizationFromWord(token.substr(1, closing - 1), kind))
        {
            return {};
        }
        // Trailing punctuation belongs to the sentence, not to the sound.
        return InlineTag(kind);
    };
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
        if (const std::string tag = vocalizationTag(token); !tag.empty())
        {
            if (!keepVocalizations)
            {
                // This backend cannot perform the sound, so the tag leaves with no
                // trace rather than becoming a word in the middle of a sentence.
                continue;
            }
            if (!output.empty())
            {
                output.push_back(' ');
            }
            output += tag;
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
                return playbackOrder.FrontReady().has_value();
            });
            if (stopToken.stop_requested())
            {
                break;
            }
            if (playbackOrder.Empty() || !enabled.load())
            {
                continue;
            }
            const std::optional<std::uint64_t> next = playbackOrder.PopFrontReady();
            if (!next.has_value()) continue;
            const std::uint64_t sequence = *next;
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
                " Using Windows voice for this phrase.",
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
        Notify({"FirstAudioPlayed", "Windows SAPI began playing the phrase.",
            ElapsedMilliseconds(utterance.queuedAt), 0, utterance.utteranceId,
            WindowsSapiResource});
        Notify({"Speaking", "Reading the assistant reply aloud.", -1.0, 0,
            utterance.utteranceId,
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

std::vector<SpeechService::Utterance> SpeechService::CollectBatchCompanions(
    const Utterance& leader)
{
    std::vector<Utterance> companions;
    if (!configuration.bQwenBatchReplyPhrases || !configuration.bQwenDirectPcm)
    {
        return companions;
    }
    // The first phrase of a reply is never batched. It is the only latency anyone
    // experiences, and collecting company for it would trade that for throughput the
    // listener does not hear.
    if (leader.latencyCritical || !leader.preset.has_value())
    {
        return companions;
    }

    const int maxPhrases = std::max(2, configuration.qwenMaxBatchPhrases);
    const int maxCharacters = std::max(64, configuration.qwenMaxBatchCharacters);
    std::size_t characters = leader.text.size();
    while (!queue.empty() &&
        static_cast<int>(companions.size()) + 1 < maxPhrases)
    {
        const Utterance& candidate = queue.front();
        // A phrase from a superseded reply, a different voice, or the start of the next
        // reply does not belong in this call. Mixing generations would let a cancelled
        // reply's text reach the card inside a batch that survives the cancellation.
        if (candidate.generation != leader.generation ||
            candidate.latencyCritical ||
            !candidate.preset.has_value() ||
            candidate.preset->id != leader.preset->id)
        {
            break;
        }
        if (characters + candidate.text.size() >
            static_cast<std::size_t>(maxCharacters))
        {
            break;
        }
        characters += candidate.text.size();
        companions.push_back(std::move(queue.front()));
        queue.pop_front();
    }
    return companions;
}

void SpeechService::VerifyInferenceBackend(
    const VoiceOperationResult& result,
    const std::uint64_t utteranceId)
{
    // Once per session, on the first phrase that actually produced audio.
    //
    // This is the check the startup line cannot make. Graph capture is deferred to the
    // first eligible phrase, so at voice load the only honest report is "installed";
    // whether a graph exists is knowable only here, from a request that ran. Without
    // this, a capture that silently failed leaves the configuration saying one thing
    // and the run doing another -- which is exactly the state the 2026-09-02 session
    // was in for 116 requests.
    if (backendVerified.exchange(true))
    {
        return;
    }
    if (!configuration.bQwenLowLatencyPhrase)
    {
        return;
    }
    const bool wantPredictorGraph = configuration.bQwenCudaGraph;
    const bool wantTalkerGraph = wantPredictorGraph && configuration.bQwenTalkerGraph;
    const auto yesNo = [](const bool value) { return value ? "yes" : "no"; };
    const std::string observed =
        std::string("backend=") +
            (result.backend.empty() ? std::string("standard") : result.backend) +
        " cuda_graph=" + yesNo(result.cudaGraph) +
        " talker_graph=" + yesNo(result.talkerGraph);

    const bool matched = result.backend == "low_latency" &&
        result.cudaGraph == wantPredictorGraph &&
        result.talkerGraph == wantTalkerGraph;
    if (matched)
    {
        Notify({"BackendVerified",
            "The first phrase ran on the configured path: " + observed,
            -1.0, 0, utteranceId, ActualQwenResource(result)});
        return;
    }
    Notify({"BackendMismatch",
        "Configured low_latency_phrase=yes predictor_graph=" +
            std::string(yesNo(wantPredictorGraph)) +
            " talker_graph=" + yesNo(wantTalkerGraph) +
            ", but the first phrase ran with " + observed +
            ". Speech is correct on this path; it is the slower one.",
        -1.0, 0, utteranceId, ActualQwenResource(result)});
}

void SpeechService::PublishGenerated(
    const Utterance& utterance,
    PreparedUtterance item,
    const int depth)
{
    const bool stale = utterance.generation != generation.load() || !enabled.load();
    const VoiceOperationResult completedResult = item.result;
    {
        std::lock_guard lock(mutex);
        if (generatingCount > 0) --generatingCount;
        if (!stale)
        {
            bufferedAudioBytes += item.bufferedBytes;
            prepared.emplace(utterance.sequence, std::move(item));
            playbackOrder.MarkReady(utterance.sequence);
        }
    }
    if (stale)
    {
        std::error_code error;
        if (!item.audioPath.empty()) std::filesystem::remove(item.audioPath, error);
    }
    else if (utterance.preset.has_value() && completedResult.succeeded)
    {
        // The conditions the request actually ran under, next to the stage timings
        // rather than in a separate place. Which card served a phrase, whether the
        // model was already there, and how many phrases were already waiting are
        // the three facts that decide whether a slow utterance was slow inference
        // or slow queueing, and none of them can be recovered afterwards.
        std::ostringstream conditions;
        conditions << (completedResult.audioCacheHit ? "Reflex audio cache hit. " : "")
            << "device=" << (completedResult.deviceName.empty()
                ? completedResult.device : completedResult.deviceName)
            << " dtype=" << completedResult.dtype
            << " attention=" << completedResult.attentionBackend
            << " resident=" << (completedResult.modelResident ? "yes" : "no")
            << " prompt_cached=" << (completedResult.clonePromptCached ? "yes" : "no")
            << " queue_depth=" << depth
            << " vram=" << completedResult.vramUsedMiB
            << '/' << completedResult.vramTotalMiB << "MiB"
            << " gpu_util=" << completedResult.gpuUtilizationPercent << '%'
            << " audio=" << completedResult.audioDurationMilliseconds << "ms"
            << " rtf=" << completedResult.realTimeFactor
                << " backend=" << (completedResult.backend.empty()
                    ? std::string("standard") : completedResult.backend)
                << " cuda_graph=" << (completedResult.cudaGraph ? "yes" : "no")
                << " talker_graph=" << (completedResult.talkerGraph ? "yes" : "no");
        SpeechEvent profile{"Profile", conditions.str(),
            completedResult.elapsedMilliseconds, depth,
            utterance.utteranceId, ActualQwenResource(completedResult)};
        profile.timings = {
            // The wait that actually happened, ahead of the worker's own lock wait.
            // The two are not interchangeable: the pool chooses a worker before the
            // worker ever sees the request, so the worker-side number is near zero by
            // construction and reading it as queue time hid a queue eight phrases deep
            // for the whole 2026-09-02 session.
            {"worker_pool_wait", completedResult.workerPoolWaitMilliseconds},
            {"python_lock_wait", completedResult.workerQueueMilliseconds},
            {"model_ready", completedResult.modelReadyMilliseconds},
            {"clone_prompt_ready", completedResult.clonePromptMilliseconds},
            {"generation", completedResult.generationMilliseconds},
            {"wav_memory_encode", completedResult.wavWriteMilliseconds},
            {"cpp_response_received", completedResult.cppResponseMilliseconds, true}
        };
        Notify(std::move(profile));
        VerifyInferenceBackend(completedResult, utterance.utteranceId);
        Notify({"FirstAudioReady", "The phrase's first playable audio is ready.",
            ElapsedMilliseconds(utterance.queuedAt), depth, utterance.utteranceId,
            ActualQwenResource(completedResult)});
        Notify({"Generated", "Qwen3-TTS phrase audio is ready for ordered playback.",
            completedResult.elapsedMilliseconds, depth, utterance.utteranceId,
            ActualQwenResource(completedResult)});
    }
    condition.notify_all();
}

bool SpeechService::SynthesizeBatch(std::vector<Utterance>& group, const int depth)
{
    if (group.size() < 2 || !group.front().preset.has_value())
    {
        return false;
    }
    const VoicePreset preset = *group.front().preset;
    std::vector<std::string> texts;
    texts.reserve(group.size());
    std::size_t characters = 0;
    for (const Utterance& utterance : group)
    {
        texts.push_back(utterance.text);
        characters += utterance.text.size();
    }

    const std::uint64_t batchGeneration = group.front().generation;
    Notify({"Generating",
        "Synthesizing " + std::to_string(group.size()) +
            " queued phrases in one batched call.",
        -1.0, depth, group.front().utteranceId, PlannedQwenResource(configuration)});

    const auto startedAt = std::chrono::steady_clock::now();
    std::vector<VoiceOperationResult> results;
    try
    {
        results = qwenPool.SynthesizePcmBatch(texts, preset);
    }
    catch (const std::exception& exception)
    {
        Notify({"BatchFallback",
            std::string("Batched synthesis threw; using the per-phrase path: ") +
                exception.what(), -1.0, depth, group.front().utteranceId});
        return false;
    }
    const double wallMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startedAt).count();

    // Anything other than one successful clip per phrase, in order, is a fallback. The
    // mapping from result to playback slot is positional, so a short or failed vector
    // cannot be partially used without risking audio played against the wrong text.
    if (results.size() != group.size())
    {
        Notify({"BatchFallback",
            results.empty()
                ? std::string("The batch returned nothing; using the per-phrase path.")
                : "The batch was declined; using the per-phrase path: " +
                    results.front().message,
            wallMilliseconds, depth, group.front().utteranceId});
        return false;
    }
    for (const VoiceOperationResult& result : results)
    {
        if (!result.succeeded)
        {
            Notify({"BatchFallback",
                "A batched phrase failed; using the per-phrase path: " + result.message,
                wallMilliseconds, depth, group.front().utteranceId});
            return false;
        }
    }

    // Cancellation between submitting the batch and receiving it. The whole group is
    // dropped rather than published: a batch is slow enough that a user who interrupted
    // during it has certainly moved on, and stale audio arriving afterwards is exactly
    // what ordered playback would otherwise let through.
    if (batchGeneration != generation.load() || !enabled.load())
    {
        std::lock_guard lock(mutex);
        if (generatingCount >= group.size()) generatingCount -= group.size();
        else generatingCount = 0;
        for (const Utterance& utterance : group)
        {
            playbackOrder.Remove(utterance.sequence);
        }
        condition.notify_all();
        Notify({"Stopped",
            "A batch of " + std::to_string(group.size()) +
                " phrases was discarded after the reply was cancelled.",
            wallMilliseconds, depth, group.front().utteranceId});
        return true;
    }

    double audioMilliseconds = 0.0;
    for (const VoiceOperationResult& result : results)
    {
        if (result.audioDurationMilliseconds > 0.0)
        {
            audioMilliseconds += result.audioDurationMilliseconds;
        }
    }
    const double generationMilliseconds = results.front().generationMilliseconds;
    const double batchFactor = audioMilliseconds > 0.0
        ? generationMilliseconds / audioMilliseconds
        : -1.0;

    {
        // What the batch actually bought, in the terms the decision was made in. A
        // real-time factor at or above one means the queue still cannot drain and the
        // ceilings need revisiting; below one it can.
        std::ostringstream summary;
        summary << "phrases=" << group.size()
            << " characters=" << characters
            << " generation_ms=" << generationMilliseconds
            << " audio_ms=" << audioMilliseconds
            << " batch_rtf=" << batchFactor
            << " peak_vram_mib=" << results.front().peakVramMiB
            << " device=" << (results.front().deviceName.empty()
                ? results.front().device : results.front().deviceName)
            << " queue_depth_before=" << depth;
        std::size_t remaining = 0;
        {
            std::lock_guard lock(mutex);
            remaining = queue.size();
        }
        summary << " queue_depth_after=" << remaining
            << " backend=" << (results.front().backend.empty()
                ? std::string("standard") : results.front().backend)
            << " cuda_graph=" << (results.front().cudaGraph ? "yes" : "no")
            << " talker_graph=" << (results.front().talkerGraph ? "yes" : "no")
            << " fallback=no";
        SpeechEvent batch{"Batch", summary.str(), generationMilliseconds,
            static_cast<int>(remaining), group.front().utteranceId,
            ActualQwenResource(results.front())};
        batch.timings = {
            {"batch_generation", generationMilliseconds},
            {"batch_audio_duration", audioMilliseconds},
            {"batch_wall_clock", wallMilliseconds, true}
        };
        Notify(std::move(batch));
    }

    for (std::size_t index = 0; index < group.size(); ++index)
    {
        PreparedUtterance item;
        item.utterance = group[index];
        item.qwenAttempted = true;
        item.result = std::move(results[index]);
        item.bufferedBytes = item.result.audioBytes.size();
        const std::uintmax_t maximumBytes =
            static_cast<std::uintmax_t>(configuration.qwenMaxBufferedAudioMiB) *
            1024U * 1024U;
        if (item.bufferedBytes > maximumBytes)
        {
            item.result.succeeded = false;
            item.result.message =
                "Generated audio exceeded the configured memory buffer limit.";
            item.result.audioBytes.clear();
            item.bufferedBytes = 0;
        }
        PublishGenerated(group[index], std::move(item), depth);
    }
    return true;
}

void SpeechService::SynthesizeOne(Utterance utterance, const int depth)
{
    PreparedUtterance item;
    item.utterance = utterance;
    if (utterance.preset.has_value())
    {
        item.qwenAttempted = true;
        std::error_code error;
        if (configuration.bQwenDirectPcm)
        {
            Notify({"Generating", "Synthesizing the phrase into bounded memory.",
                -1.0, depth, utterance.utteranceId,
                PlannedQwenResource(configuration)});
            item.result = qwenPool.SynthesizePcm(
                utterance.text, *utterance.preset, utterance.latencyCritical);
        }
        else
        {
            item.audioPath = std::filesystem::absolute(
                presetStore.Root() / "Playback" /
                    (utterance.preset->id + "-" +
                     std::to_string(utterance.generation) + "-" +
                     std::to_string(utterance.sequence) + ".wav"), error).lexically_normal();
            if (error)
            {
                item.result = {false, "The Qwen playback path could not be resolved.",
                    {}, -1.0};
            }
            else
            {
                std::filesystem::create_directories(item.audioPath.parent_path(), error);
                Notify({"Generating", "Synthesizing a phrase ahead with Qwen3-TTS.",
                    -1.0, depth, utterance.utteranceId,
                    PlannedQwenResource(configuration)});
                item.result = qwenPool.Synthesize(
                    utterance.text, *utterance.preset, item.audioPath.string(),
                    utterance.latencyCritical);
            }
        }
        if (item.result.succeeded)
        {
            item.bufferedBytes = configuration.bQwenDirectPcm
                ? item.result.audioBytes.size()
                : std::filesystem::file_size(item.audioPath, error);
            if (error) item.bufferedBytes = 0;
            const std::uintmax_t maximumBytes =
                static_cast<std::uintmax_t>(configuration.qwenMaxBufferedAudioMiB) *
                1024U * 1024U;
            if (item.bufferedBytes > maximumBytes)
            {
                item.result.succeeded = false;
                item.result.message =
                    "Generated audio exceeded the configured memory buffer limit.";
                item.result.audioBytes.clear();
                item.bufferedBytes = 0;
            }
        }
    }

    PublishGenerated(utterance, std::move(item), depth);
}

void SpeechService::Generate(const std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        Utterance utterance;
        std::vector<Utterance> companions;
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
            // Taken under the same lock that took the leader, so no other generator can
            // claim a phrase that is about to be batched with this one.
            companions = CollectBatchCompanions(utterance);
            generatingCount += companions.size();
            depth = static_cast<int>(queue.size());
        }

        if (!companions.empty())
        {
            std::vector<Utterance> group;
            group.reserve(companions.size() + 1);
            group.push_back(utterance);
            for (Utterance& companion : companions)
            {
                group.push_back(std::move(companion));
            }
            if (SynthesizeBatch(group, depth))
            {
                continue;
            }
            // Fallback. The group is already off the queue and already counted as
            // generating, so it is finished here one phrase at a time rather than put
            // back -- returning it would race another generator for phrases whose
            // playback slots are already reserved.
            for (Utterance& member : group)
            {
                SynthesizeOne(std::move(member), depth);
            }
            continue;
        }

        SynthesizeOne(std::move(utterance), depth);
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
    Notify({"FirstAudioPlayed", "Ordered Qwen3-TTS playback began.",
        ElapsedMilliseconds(utterance.queuedAt), 0, utterance.utteranceId,
        ActualQwenResource(generated)});
    Notify(std::move(playing));
    const bool inMemory = !generated.audioBytes.empty();
    const std::wstring diskPath = inMemory ? std::wstring{} : output.wstring();
    const wchar_t* sound = inMemory
        ? reinterpret_cast<const wchar_t*>(generated.audioBytes.data())
        : diskPath.c_str();
    const DWORD flags = (inMemory ? SND_MEMORY : SND_FILENAME) |
        SND_ASYNC | SND_NODEFAULT;
    if (!PlaySoundW(sound, nullptr, flags))
    {
        Notify({"Fallback", "Windows could not play the Qwen3-TTS WAV; using SAPI.",
            -1.0, 0, utterance.utteranceId, WindowsSapiResource});
        return false;
    }
    ArmBargeIn();
    const double parsedDuration = inMemory
        ? WavDurationMilliseconds(generated.audioBytes)
        : WavDurationMilliseconds(output);
    const double duration = std::max(250.0,
        generated.audioDurationMilliseconds > 0.0
            ? generated.audioDurationMilliseconds
            : parsedDuration);
    const auto deadline = std::chrono::steady_clock::now() +
        // PlaySound is asynchronous. Leave a small tail before the next ordered phrase
        // starts, otherwise timer granularity or a non-canonical WAV header can make the
        // next phrase cut the last word off the current one.
        std::chrono::milliseconds(static_cast<long long>(duration + 350.0));
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
    if (!inMemory) std::filesystem::remove(output, error);
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
