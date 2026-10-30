#include "Speech/fileTranscription.h"

#include "Core/localApiKey.h"
#include "Core/runtimePath.h"
#include "Core/utf8.h"
#include "Process/ownedProcess.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace revia::speech
{
namespace
{
constexpr std::size_t MaximumWaveBytes = 2 * 1024 * 1024;
constexpr std::size_t MaximumJsonBytes = 256 * 1024;
constexpr std::size_t MaximumTranscriptBytes = 64 * 1024;

std::string PathText(const std::filesystem::path& path)
{
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::filesystem::path Installed(const std::string& configured)
{
    const auto path = std::filesystem::path(std::u8string(configured.begin(), configured.end()));
    auto found = path.is_absolute() ? path : core::FindInstalledArtifact(core::RuntimeRoot(), path);
    if (found.empty() && !path.is_absolute())
        found = core::ResolveRuntimePath(path);
    std::error_code error;
    if (found.empty() || !std::filesystem::is_regular_file(found, error) || error)
        return {};
    return std::filesystem::canonical(found, error);
}

bool ReadBounded(const std::filesystem::path& path, const std::size_t maximum, std::string& bytes)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return false;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > maximum)
        return false;
    std::ifstream input(path, std::ios::binary);
    bytes.resize(static_cast<std::size_t>(size));
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return input.gcount() == static_cast<std::streamsize>(bytes.size()) && input.peek() == std::char_traits<char>::eof() && !input.bad();
}

bool ResultFileWithinLimit(const std::filesystem::path& path, std::string& outError)
{
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
        return true;
    if (error || !std::filesystem::is_regular_file(status))
    {
        outError = "The transcription result file is not a regular local file.";
        return false;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > MaximumJsonBytes)
    {
        outError = "The transcription result file exceeded its size limit or could not be inspected.";
        return false;
    }
    return true;
}

std::uint32_t Little(const std::string& bytes, const std::size_t offset, const int count)
{
    std::uint32_t value = 0;
    for (int index = 0; index < count; ++index)
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + index])) << (index * 8);
    return value;
}

std::optional<std::int64_t> WaveDuration(const std::string& bytes)
{
    if (bytes.size() < 44 || bytes.compare(0, 4, "RIFF") != 0 || bytes.compare(8, 4, "WAVE") != 0 ||
        static_cast<std::uint64_t>(Little(bytes, 4, 4)) + 8 != bytes.size())
        return {};
    bool format = false;
    std::uint32_t dataBytes = 0;
    for (std::size_t at = 12; at < bytes.size();)
    {
        if (bytes.size() - at < 8)
            return {};
        const auto size = Little(bytes, at + 4, 4);
        if (size > bytes.size() - at - 8)
            return {};
        const std::size_t body = at + 8;
        if (bytes.compare(at, 4, "fmt ") == 0)
        {
            if (format || (size != 16 && size != 18) || Little(bytes, body, 2) != 1 || Little(bytes, body + 2, 2) != 1 ||
                Little(bytes, body + 4, 4) != 16000 || Little(bytes, body + 8, 4) != 32000 || Little(bytes, body + 12, 2) != 2 ||
                Little(bytes, body + 14, 2) != 16 || (size == 18 && Little(bytes, body + 16, 2) != 0))
                return {};
            format = true;
        }
        else if (bytes.compare(at, 4, "data") == 0)
        {
            if (dataBytes != 0 || size == 0 || size % 2 != 0 || size > 1920000)
                return {};
            dataBytes = size;
        }
        at = body + size + (size % 2);
        if (at > bytes.size())
            return {};
    }
    return format && dataBytes ? std::optional<std::int64_t>((dataBytes / 2 + 15) / 16) : std::nullopt;
}

struct TemporaryFiles
{
    std::filesystem::path directory;

    bool Create(const std::filesystem::path& parent)
    {
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const auto candidate = parent / (".revia-transcribe-" + core::GenerateLocalApiKey().substr(6, 24));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error))
            {
                directory = candidate;
                return true;
            }
            if (error)
                return false;
        }
        return false;
    }

    bool Clean()
    {
        if (directory.empty())
            return true;
        std::error_code error;
        for (const auto* name : {"input.wav", "result.json"})
        {
            std::filesystem::remove(directory / name, error);
            if (error)
                return false;
        }
        std::filesystem::remove(directory, error);
        if (error)
            return false;
        directory.clear();
        return true;
    }

    ~TemporaryFiles()
    {
        Clean();
    }
};

bool RunWhisper(const std::filesystem::path& executable, const std::filesystem::path& model, const std::filesystem::path& vadModel,
    const TemporaryFiles& files, const speechRecognitionSettings& settings, const std::stop_token stopToken,
    FileTranscriptionResult& result)
{
    process::OwnedProcess child;
    process::OwnedProcessRequest request;
    request.executable = executable;
    request.arguments = {"-m", PathText(model), "-f", PathText(files.directory / "input.wav"), "-l", settings.language, "-oj", "-of",
        PathText(files.directory / "result"), "-t", std::to_string(settings.threads)};
    if (!vadModel.empty())
        request.arguments.insert(request.arguments.end(),
            {"--vad", "--vad-model", PathText(vadModel), "--vad-threshold", "0.50", "--vad-min-speech-duration-ms", "250",
                "--vad-min-silence-duration-ms", "100", "--vad-speech-pad-ms", "30", "--vad-samples-overlap", "0.10"});
    if (!settings.bUseGpu || settings.device == "cpu")
    {
        request.arguments.push_back("-ng");
        request.environment["CUDA_VISIBLE_DEVICES"] = "-1";
    }
    else if (settings.device.starts_with("cuda:"))
    {
        const auto ordinal = settings.device.substr(5);
        if (ordinal.empty() || ordinal.size() > 2 ||
            !std::all_of(ordinal.begin(), ordinal.end(), [](const auto ch) { return ch >= '0' && ch <= '9'; }))
        {
            result.error = "The file transcription device is invalid.";
            return false;
        }
        request.arguments.insert(request.arguments.end(), {"-dev", ordinal});
    }
    request.maxPendingOutputBytes = MaximumTranscriptBytes;
    request.maxErrorBytes = 16384;
    if (!child.Start(request, result.error))
        return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(settings.requestTimeoutSeconds);
    std::size_t outputBytes = 0;
    while (child.Running() || !child.ExitCode())
    {
        if (stopToken.stop_requested())
        {
            child.Terminate();
            result.cancelled = true;
            result.error = "File transcription was cancelled.";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            child.Terminate();
            result.error = "File transcription exceeded its time limit.";
            return false;
        }
        outputBytes += child.Read(std::chrono::milliseconds(20)).size();
        if (!ResultFileWithinLimit(files.directory / "result.json", result.error))
        {
            child.Terminate();
            return false;
        }
        if (outputBytes > MaximumTranscriptBytes)
        {
            child.Terminate();
            result.error = "The transcription process exceeded its output limit.";
            return false;
        }
    }
    if (!child.StopReason().empty())
    {
        result.error = "The transcription process exceeded its output limit.";
        return false;
    }
    if (child.ExitCode() != 0)
    {
        result.error = "whisper.cpp exited before producing a transcript.";
        return false;
    }
    return true;
}

std::optional<std::int64_t> Offset(const nlohmann::json& value, const std::int64_t duration)
{
    if (!value.is_number_integer())
        return {};
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>() <= static_cast<std::uint64_t>(duration) ? std::optional<std::int64_t>(value.get<std::int64_t>())
                                                                                  : std::nullopt;
    const auto offset = value.get<std::int64_t>();
    return offset >= 0 && offset <= duration ? std::optional(offset) : std::nullopt;
}

bool ParseTranscript(const std::string& bytes, const std::int64_t duration, FileTranscriptionResult& result)
{
    const auto json = nlohmann::json::parse(bytes);
    if (!json.contains("transcription") || !json["transcription"].is_array() || json["transcription"].size() > 512)
        return false;
    std::size_t textBytes = 0;
    std::int64_t previous = 0;
    for (const auto& item : json["transcription"])
    {
        if (!item.is_object() || !item.contains("offsets") || !item["offsets"].is_object() || !item["offsets"].contains("from") ||
            !item["offsets"].contains("to") || !item.contains("text") || !item["text"].is_string())
            return false;
        const auto start = Offset(item["offsets"]["from"], duration);
        const auto end = Offset(item["offsets"]["to"], duration);
        std::string text = item["text"].get<std::string>();
        textBytes += text.size();
        if (!start || !end || *end < *start || *start < previous || textBytes > MaximumTranscriptBytes || !utf8::IsValid(text) ||
            std::any_of(text.begin(), text.end(), [](const unsigned char ch) { return ch < 32 && ch != '\r' && ch != '\n' && ch != '\t'; }))
            return false;
        previous = *start;
        const auto first = text.find_first_not_of(" \t\r\n");
        const auto last = text.find_last_not_of(" \t\r\n");
        text = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        if (!text.empty() && text != "[BLANK_AUDIO]")
            result.segments.push_back({*start, *end, std::move(text)});
    }
    return true;
}
}

FileTranscriptionResult TranscribeWaveFile(const std::filesystem::path& wavePath, const speechRecognitionSettings& settings,
    const std::stop_token stopToken, const std::filesystem::path& vadModelPath)
{
    FileTranscriptionResult result;
    TemporaryFiles files;
    try
    {
        if (stopToken.stop_requested())
        {
            result.cancelled = true;
            result.error = "File transcription was cancelled.";
            return result;
        }
        std::string wave;
        if (!ReadBounded(wavePath, MaximumWaveBytes, wave))
            throw std::runtime_error("The transcription input must be a local WAV no larger than 2 MiB.");
        const auto duration = WaveDuration(wave);
        if (!duration)
            throw std::runtime_error("File transcription requires nonempty 16 kHz mono PCM16 WAV audio of at most 60 seconds.");
        if (settings.threads < 1 || settings.threads > 64 || settings.requestTimeoutSeconds < 1 || settings.requestTimeoutSeconds > 180 ||
            settings.language.empty() || settings.language.size() > 16 ||
            !std::all_of(settings.language.begin(), settings.language.end(),
                [](const unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-'; }))
            throw std::runtime_error("File transcription settings exceed the bounded worker contract.");
        const auto executable = Installed(settings.executable);
        const auto model = Installed(settings.modelPath);
        if (executable.empty() || model.empty())
            throw std::runtime_error("The configured whisper.cpp CLI or local recognition model is unavailable.");
        const auto vadModel = vadModelPath.empty() ? std::filesystem::path{} : Installed(PathText(vadModelPath));
        if (!vadModelPath.empty())
        {
            std::error_code vadError;
            if (vadModel.empty() || std::filesystem::file_size(vadModel, vadError) == 0 || vadError)
                throw std::runtime_error("The explicitly requested local VAD model is unavailable or empty.");
        }
        std::error_code error;
        const auto input = std::filesystem::canonical(wavePath, error);
        if (error || !files.Create(input.parent_path()))
            throw std::runtime_error("The owned file-transcription temporary directory could not be created.");
        {
            std::ofstream snapshot(files.directory / "input.wav", std::ios::binary);
            snapshot.write(wave.data(), static_cast<std::streamsize>(wave.size()));
            if (!snapshot)
                throw std::runtime_error("The validated transcription input could not be staged.");
        }
        if (RunWhisper(executable, model, vadModel, files, settings, stopToken, result))
        {
            std::string bytes;
            if (!ReadBounded(files.directory / "result.json", MaximumJsonBytes, bytes) || !ParseTranscript(bytes, *duration, result))
                throw std::runtime_error("whisper.cpp returned invalid or out-of-bounds timestamped transcription data.");
            result.ok = true;
        }
    }
    catch (const std::exception& exception)
    {
        result.error = utf8::Prefix(utf8::Sanitize(exception.what()), 512);
        result.ok = false;
    }
    if (stopToken.stop_requested())
    {
        result.ok = false;
        result.cancelled = true;
        result.error = "File transcription was cancelled.";
    }
    if (!files.Clean())
    {
        result.ok = false;
        result.error = "The owned transcription results could not be removed.";
    }
    if (!result.ok)
        result.segments.clear();
    return result;
}
}
