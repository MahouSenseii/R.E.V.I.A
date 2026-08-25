#include "Runtime/runtimeDataBootstrap.h"

#include <array>
#include <system_error>

namespace revia::runtime
{

namespace
{
    bool EnsureDirectory(const std::filesystem::path& path, std::string& errorMessage)
    {
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error)
        {
            errorMessage = "Could not create runtime directory " + path.string() +
                ": " + error.message();
            return false;
        }
        if (!std::filesystem::is_directory(path, error) || error)
        {
            errorMessage = "Runtime path is not a directory: " + path.string();
            return false;
        }
        return true;
    }

    bool CopySeedIfMissing(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        bool& copied,
        std::string& errorMessage)
    {
        copied = false;
        std::error_code error;
        if (std::filesystem::exists(destination, error) && !error)
        {
            return true;
        }
        error.clear();
        if (!std::filesystem::is_regular_file(source, error) || error)
        {
            errorMessage = "Default runtime seed is missing: " + source.string();
            return false;
        }
        if (!EnsureDirectory(destination.parent_path(), errorMessage))
        {
            return false;
        }
        error.clear();
        copied = std::filesystem::copy_file(
            source, destination, std::filesystem::copy_options::none, error);
        if (copied)
        {
            return true;
        }
        // A second Revia process may have completed the same first-run copy.
        error.clear();
        if (std::filesystem::exists(destination, error) && !error)
        {
            return true;
        }
        errorMessage = "Could not seed " + destination.string() + " from " +
            source.string() + ": " + error.message();
        return false;
    }
}

RuntimeDataBootstrapResult BootstrapRuntimeData(
    const appSettings& settings,
    const std::filesystem::path& runtimeRoot,
    const std::filesystem::path& seedRoot)
{
    RuntimeDataBootstrapResult result;
    const std::array directories = {
        runtimeRoot,
        runtimeRoot / "Capabilities",
        runtimeRoot / "Browser",
        runtimeRoot / "Browser" / "Profile",
        runtimeRoot / "Initiative",
        std::filesystem::path(settings.speech.voiceDataPath),
        std::filesystem::path(settings.llm.mediaPath)
    };
    for (const auto& directory : directories)
    {
        if (!EnsureDirectory(directory, result.error))
        {
            return result;
        }
    }

    const std::filesystem::path seedVoices = seedRoot / "Voices";
    const std::filesystem::path runtimeVoices = settings.speech.voiceDataPath;
    bool referenceCopied = false;
    if (!CopySeedIfMissing(
            seedVoices / "revia-bright" / "reference.wav",
            runtimeVoices / "revia-bright" / "reference.wav",
            referenceCopied,
            result.error))
    {
        return result;
    }

    bool catalogCopied = false;
    if (!CopySeedIfMissing(
            seedVoices / "voices.json",
            runtimeVoices / "voices.json",
            catalogCopied,
            result.error))
    {
        return result;
    }

    result.succeeded = true;
    result.defaultVoiceSeeded = referenceCopied || catalogCopied;
    return result;
}

}
