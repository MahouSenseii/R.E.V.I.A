#pragma once

#include "Actions/actionRuntime.h"
#include "Agents/turnCoordinator.h"
#include "Core/commandManager.h"
#include "Core/configManager.h"
#include "Core/conversationContext.h"
#include "Core/logger.h"
#include "Core/messageRouter.h"
#include "LLM/LLamaCPP/llamaCppServerProcess.h"
#include "Runtime/affectController.h"
#include "Runtime/runtimeEvents.h"
#include "Speech/speechService.h"
#include "Speech/speechRecognitionService.h"
#include "Vision/screenCaptureService.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>

namespace revia::runtime
{

struct SessionResult
{
    bool succeeded = true;
    bool shouldExit = false;
    bool wasStreamed = false;
    bool fromAssistant = false;
    std::string text;
    std::string reason;
};

class ReviaSession
{
public:
    using ConfirmationHandler = std::function<bool(
        const actions::ActionRequest&,
        const actions::PolicyDecision&)>;

    ReviaSession();
    ~ReviaSession();

    ReviaSession(const ReviaSession&) = delete;
    ReviaSession& operator=(const ReviaSession&) = delete;

    bool Start();
    SessionResult Submit(const std::string& input);
    void PollBackgroundEvents();
    void RequestStop();
    void Stop();

    void SetConfirmationHandler(ConfirmationHandler handler);
    RuntimeEventBus& Events();
    RuntimeState State() const;
    bool IsStarted() const;
    bool IsBusy() const;
    bool IsSpeechEnabled() const;
    void SetSpeechEnabled(bool enabled);
    bool BeginListening();
    bool EndListening();
    SessionResult AnalyzeScreen(const std::string& prompt);
    std::string DisplayName() const;
    std::string Greeting() const;
    speech::VoiceStudioSnapshot VoiceStudio() const;
    speech::VoiceOperationResult CreateVoicePreset(
        const std::string& name,
        const std::string& description,
        const std::string& referenceText,
        const std::string& language);
    speech::VoiceOperationResult PreviewVoice(
        const std::string& presetId,
        const std::string& text);
    speech::VoiceOperationResult AssignVoice(
        const std::string& profileId,
        const std::string& presetId);

private:
    bool EnsureLLMAvailable(std::stop_token stopToken);
    bool EnsureEmbeddingAvailable(std::stop_token stopToken);
    bool TryHandleActionInput(const std::string& input, SessionResult& result);
    SessionResult ExecuteAction(actions::ActionRequest request);
    static std::string FormatActionOutcome(const actions::ActionOutcome& outcome);
    std::stop_token BeginOperation();
    void SetState(RuntimeState newState, const std::string& activity = "");
    void PublishAffect(const AffectSnapshot& affect);
    void Publish(RuntimeEventKind kind, const std::string& message, std::uint64_t turnId = 0) const;

    RuntimeEventBus eventBus;
    logger appLogger;
    messageRouter router;
    configManager config;
    commandManager commands;
    conversationContext context;
    appSettings settings;
    aiProfile profile;
    actions::ActionRuntime actionRuntime;
    AffectController affectController;
    speech::SpeechService speechService;
    speech::SpeechRecognitionService speechRecognitionService;
    vision::ScreenCaptureService screenCaptureService;
    llamaCppServerProcess llamaServerProcess;
    llamaCppServerProcess embeddingServerProcess;
    agents::TurnCoordinator turnCoordinator;

    mutable std::mutex operationMutex;
    mutable std::mutex cancellationMutex;
    mutable std::mutex confirmationMutex;
    mutable std::mutex voiceStudioMutex;
    std::stop_source activeStopSource;
    ConfirmationHandler confirmationHandler;
    std::atomic<RuntimeState> state = RuntimeState::Offline;
    std::atomic<bool> started = false;
    std::atomic<bool> busy = false;
    bool llmAvailable = false;
    std::uint64_t turnCounter = 0;
};

} // namespace revia::runtime
