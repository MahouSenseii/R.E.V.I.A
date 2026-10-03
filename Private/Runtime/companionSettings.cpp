#include "Runtime/companionSettings.h"
#include "Runtime/companion.h"
#include "Core/logger.h"

#include <filesystem>
#include <string>

namespace revia::runtime
{
namespace
{
std::string SharedPath(const std::string& configured, const CompanionPaths& paths, const bool executable = false)
{
    if (configured.empty())
        return {};
    const std::filesystem::path value(configured);
    if (executable && !value.has_parent_path())
        return configured;
    return (value.is_absolute() ? value : paths.InstallRoot() / value).lexically_normal().string();
}

std::string PrivatePath(const std::string& configured, const std::filesystem::path& fallback, const CompanionPaths& paths)
{
    if (!paths.Descriptor().legacy)
        return paths.Resolve(fallback).string();
    const std::filesystem::path value(configured.empty() ? fallback : std::filesystem::path(configured));
    return (value.is_absolute() ? value : paths.InstallRoot() / value).lexically_normal().string();
}
}

std::filesystem::path CompanionLogDirectory(const CompanionPaths& paths)
{
    return paths.Descriptor().legacy ? PrivatePath(ReviaLogDirectory(), "Logs", paths) : paths.Resolve("Logs").string();
}

void BindCompanionSettings(appSettings& settings, const CompanionPaths& paths)
{
    settings.llm.serverExecutable = SharedPath(settings.llm.serverExecutable, paths, true);
    settings.llm.modelPath = SharedPath(settings.llm.modelPath, paths);
    settings.llm.multimodalProjectorPath = SharedPath(settings.llm.multimodalProjectorPath, paths);
    settings.llm.mediaPath = PrivatePath(settings.llm.mediaPath, "RuntimeData/Vision", paths);
    if (!paths.Descriptor().legacy)
        settings.llm.bAllowPromptCache = false;
    settings.embedding.serverExecutable = SharedPath(settings.embedding.serverExecutable, paths, true);
    settings.embedding.modelPath = SharedPath(settings.embedding.modelPath, paths);
    for (modelTierSettings* tier : {&settings.intelligence.fast, &settings.intelligence.expert})
    {
        tier->modelPath = SharedPath(tier->modelPath, paths);
        tier->multimodalProjectorPath = SharedPath(tier->multimodalProjectorPath, paths);
    }
    settings.speech.pythonExecutable = SharedPath(settings.speech.pythonExecutable, paths, true);
    settings.speech.qwenServiceScript = SharedPath(settings.speech.qwenServiceScript, paths);
    settings.speech.voiceDataPath = PrivatePath(settings.speech.voiceDataPath, "RuntimeData/Voices", paths);
    settings.speechRecognition.executable = SharedPath(settings.speechRecognition.executable, paths, true);
    settings.speechRecognition.serverExecutable = SharedPath(settings.speechRecognition.serverExecutable, paths, true);
    settings.speechRecognition.modelPath = SharedPath(settings.speechRecognition.modelPath, paths);
    settings.speechRecognition.dataDirectory = paths.Resolve("RuntimeData/SpeechInput").string();
    settings.image.pythonExecutable = SharedPath(settings.image.pythonExecutable, paths, true);
    settings.image.serviceScript = SharedPath(settings.image.serviceScript, paths);
    settings.image.cacheDirectory = SharedPath(settings.image.cacheDirectory, paths);
    settings.image.outputPath = PrivatePath(settings.image.outputPath, "RuntimeData/Images", paths);
    settings.performance.songLibraryPath = PrivatePath(settings.performance.songLibraryPath, "RuntimeData/Songs", paths);
    settings.presence.statePath = PrivatePath(settings.presence.statePath, "RuntimeData/Presence/avatar_state.json", paths);
    settings.presence.eventPath = PrivatePath(settings.presence.eventPath, "RuntimeData/Presence/avatar_events.jsonl", paths);
    settings.presence.inboxPath = PrivatePath(settings.presence.inboxPath, "RuntimeData/Presence/Inbox", paths);
    settings.presence.outboxPath = PrivatePath(settings.presence.outboxPath, "RuntimeData/Presence/Outbox", paths);
    settings.improvement.proposalsPath = PrivatePath(settings.improvement.proposalsPath, "RuntimeData/Improvement/Proposals", paths);
    if (!paths.Descriptor().legacy)
    {
        settings.improvement.workbenchPath = paths.Resolve("RuntimeData/Improvement/Workbench").string();
    }
    const std::string logDirectory = CompanionLogDirectory(paths).string();
    settings.llm.logDirectory = logDirectory;
    settings.embedding.logDirectory = logDirectory;
    settings.speech.logDirectory = logDirectory;
    settings.speechRecognition.logDirectory = logDirectory;
    settings.image.logDirectory = logDirectory;
}
}
