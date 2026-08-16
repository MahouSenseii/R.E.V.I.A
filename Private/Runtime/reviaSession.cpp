#include "Runtime/reviaSession.h"
#include "Core/localApiKey.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>
#include <utility>

namespace revia::runtime
{

namespace
{
    double ElapsedMilliseconds(const std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }

    std::string BackendCapacity(const healthOutput& health)
    {
        if (health.contextTokens <= 0)
        {
            return {};
        }
        std::ostringstream stream;
        stream << " Effective context: " << health.contextTokens << " tokens";
        if (health.parallelSlots > 0)
        {
            stream << " per slot across " << health.parallelSlots <<
                (health.parallelSlots == 1 ? " slot." : " slots.");
        }
        else
        {
            stream << '.';
        }
        if (health.responseTokenLimit > 0)
        {
            stream << " Adaptive response limit: " << health.responseTokenLimit << " tokens.";
        }
        return stream.str();
    }
}

ReviaSession::ReviaSession()
{
    appLogger.SetSink([this](const std::string& line)
    {
        Publish(RuntimeEventKind::Activity, line);
    });
}

ReviaSession::~ReviaSession()
{
    Stop();
}

bool ReviaSession::Start()
{
    std::lock_guard operationLock(operationMutex);
    if (started.load())
    {
        return true;
    }

    busy.store(true);
    const std::stop_token stopToken = BeginOperation();
    const auto startupStarted = std::chrono::steady_clock::now();
    std::vector<latencySample> startupTimings;
    SetState(RuntimeState::Starting, "Starting Revia core.");
    appLogger.Log("Starting core...");

    auto stageStarted = std::chrono::steady_clock::now();
    const bool settingsLoaded = config.LoadSettings(settings);
    startupTimings.push_back({"settings_load", ElapsedMilliseconds(stageStarted)});
    if (!settingsLoaded)
    {
        appLogger.Error("Failed to load settings.");
        startupTimings.push_back({"startup_total", ElapsedMilliseconds(startupStarted), true});
        appLogger.Timing("startup", startupTimings);
        busy.store(false);
        SetState(RuntimeState::Error, "Settings could not be loaded.");
        return false;
    }
    if (settings.llm.apiKey.empty())
    {
        settings.llm.apiKey = core::GenerateLocalApiKey();
    }
    if (settings.embedding.apiKey.empty())
    {
        settings.embedding.apiKey = core::GenerateLocalApiKey();
    }

    stageStarted = std::chrono::steady_clock::now();
    const bool profileLoaded = config.LoadProfile(settings.activeProfile, profile);
    startupTimings.push_back({"profile_load", ElapsedMilliseconds(stageStarted)});
    if (!profileLoaded)
    {
        appLogger.Error("Failed to load profile: " + settings.activeProfile);
        startupTimings.push_back({"startup_total", ElapsedMilliseconds(startupStarted), true});
        appLogger.Timing("startup", startupTimings);
        busy.store(false);
        SetState(RuntimeState::Error, "The active profile could not be loaded.");
        return false;
    }

    std::string actionError;
    stageStarted = std::chrono::steady_clock::now();
    if (!actionRuntime.Initialize("Config/capabilities.json", "Audit/actions.jsonl", actionError))
    {
        appLogger.Warning("Action runtime disabled: " + actionError);
    }
    else
    {
        appLogger.Log("Capability runtime initialized.");
    }
    startupTimings.push_back({"action_runtime_init", ElapsedMilliseconds(stageStarted)});

    stageStarted = std::chrono::steady_clock::now();
    speechService.SetActiveProfile(profile.id);
    speechService.Start(settings.speech, [this](const speech::SpeechEvent& speechEvent)
    {
        RuntimeEvent event;
        event.kind = RuntimeEventKind::ComponentStatus;
        event.state = state.load();
        event.message = speechEvent.detail;
        event.component = "Voice";
        event.phase = speechEvent.phase;
        event.elapsedMilliseconds = speechEvent.elapsedMilliseconds;
        event.queueDepth = speechEvent.queueDepth;
        eventBus.Publish(std::move(event));
    });
    startupTimings.push_back({"speech_service_init", ElapsedMilliseconds(stageStarted)});

    if (speechService.HasActiveQwenVoice())
    {
        stageStarted = std::chrono::steady_clock::now();
        const speech::VoiceOperationResult preparedVoice = speechService.PrepareActiveVoice();
        startupTimings.push_back({"qwen_voice_model_load", ElapsedMilliseconds(stageStarted)});
        if (preparedVoice.succeeded)
        {
            appLogger.Log(preparedVoice.message +
                " llama.cpp will fit around the voice model's current GPU allocation.");
        }
        else
        {
            appLogger.Warning("Assigned Qwen voice could not load: " + preparedVoice.message);
        }
    }

    stageStarted = std::chrono::steady_clock::now();
    speechRecognitionService.Start(
        settings.speechRecognition,
        [this](const speech::RecognitionEvent& recognitionEvent)
        {
            RuntimeEvent event;
            event.kind = RuntimeEventKind::ComponentStatus;
            event.state = state.load();
            event.message = recognitionEvent.transcript.empty()
                ? recognitionEvent.detail
                : recognitionEvent.transcript;
            event.component = "Microphone";
            event.phase = recognitionEvent.phase;
            event.elapsedMilliseconds = recognitionEvent.elapsedMilliseconds;
            eventBus.Publish(std::move(event));
        });
    startupTimings.push_back({"speech_recognition_init", ElapsedMilliseconds(stageStarted)});

    stageStarted = std::chrono::steady_clock::now();
    router.ApplyLLMSettings(settings.llm, settings.embedding, profile);
    if (settings.llm.bAutoTune)
    {
        appLogger.Log(
            "Performance mode is automatic: Revia will size one chat context from GPU and "
            "system memory, while llama.cpp fits GPU layers and flash attention. "
            "Response limit scales up to " +
            std::to_string(settings.llm.maxTokens) + " tokens from the effective context.");
    }
    else
    {
        appLogger.Log("Performance mode is manual: context and server slots come from settings.json.");
    }
    startupTimings.push_back({"llm_configuration", ElapsedMilliseconds(stageStarted)});

    stageStarted = std::chrono::steady_clock::now();
    llmAvailable = EnsureLLMAvailable(stopToken);
    startupTimings.push_back({"llm_health_or_start", ElapsedMilliseconds(stageStarted)});

    stageStarted = std::chrono::steady_clock::now();
    const bool embeddingAvailable = EnsureEmbeddingAvailable(stopToken);
    startupTimings.push_back({"embedding_health_or_start", ElapsedMilliseconds(stageStarted)});
    if (embeddingAvailable && profile.bMemoryEnabled && !stopToken.stop_requested())
    {
        stageStarted = std::chrono::steady_clock::now();
        turnCoordinator.BackfillMemoryEmbeddings(router, settings.embedding.modelName);
        startupTimings.push_back({"embedding_backfill_queue", ElapsedMilliseconds(stageStarted)});
    }

    startupTimings.push_back({"startup_total", ElapsedMilliseconds(startupStarted), true});
    appLogger.Log("Loaded profile: " + profile.displayName);
    PublishAffect(affectController.Reset());
    appLogger.Timing("startup", startupTimings);
    busy.store(false);

    if (stopToken.stop_requested())
    {
        SetState(RuntimeState::Offline, "Startup was stopped.");
        return false;
    }

    started.store(true);
    if (llmAvailable)
    {
        SetState(RuntimeState::Idle, profile.displayName + " is online.");
    }
    else
    {
        SetState(RuntimeState::Error, "The language model is unavailable; typed actions remain available.");
    }
    RuntimeEvent visionEvent;
    visionEvent.kind = RuntimeEventKind::ComponentStatus;
    visionEvent.state = state.load();
    visionEvent.component = "Vision";
    visionEvent.phase = !settings.vision.bEnabled ? "Disabled" :
        llmAvailable ? "Ready" : "Unavailable";
    visionEvent.message = !settings.vision.bEnabled
        ? "Local screen vision is off."
        : llmAvailable
            ? "Local opt-in screen analysis is ready."
            : "Vision requires the configured multimodal llama.cpp server.";
    eventBus.Publish(std::move(visionEvent));
    if (settings.speech.bEnabled && settings.speech.bSpeakGreeting && !Greeting().empty())
    {
        speechService.Speak(Greeting(), affectController.Current());
    }
    return true;
}

SessionResult ReviaSession::Submit(const std::string& input)
{
    std::lock_guard operationLock(operationMutex);
    SessionResult result;
    if (!started.load())
    {
        result.succeeded = false;
        result.text = "Revia is not ready yet.";
        result.reason = "The runtime session has not started.";
        return result;
    }
    if (input.empty())
    {
        result.succeeded = false;
        result.text = "I didn't hear anything.";
        result.reason = "Input was empty.";
        return result;
    }

    busy.store(true);
    const std::stop_token stopToken = BeginOperation();
    const auto finish = [&](SessionResult finished)
    {
        if (!finished.shouldExit)
        {
            const AffectSnapshot affect = affectController.ObserveTurn(
                input,
                finished.text,
                finished.succeeded);
            PublishAffect(affect);
            if (finished.fromAssistant && finished.succeeded && !finished.text.empty())
            {
                speechService.Speak(finished.text, affect);
            }
        }
        busy.store(false);
        return finished;
    };

    if (router.IsExitCommand(input))
    {
        result.shouldExit = true;
        result.text = "Exiting R.E.V.I.A...";
        return finish(std::move(result));
    }

    if (TryHandleActionInput(input, result))
    {
        return finish(std::move(result));
    }

    const commandOutput commandResult = commands.HandleCommand(
        input,
        settings,
        profile,
        config,
        router);
    if (commandResult.bWasCommand)
    {
        result.succeeded = commandResult.bSuccess;
        result.shouldExit = commandResult.bShouldExit;
        result.text = commandResult.output;
        result.reason = commandResult.reason;
        if (!result.succeeded)
        {
            SetState(RuntimeState::Blocked, result.reason);
        }
        else
        {
            SetState(RuntimeState::Idle);
        }
        return finish(std::move(result));
    }

    context.AddMessage("user", input);
    const std::uint64_t currentTurn = ++turnCounter;
    const auto turnStarted = std::chrono::steady_clock::now();
    SetState(RuntimeState::Thinking, "Thinking about turn #" + std::to_string(currentTurn) + ".");

    const agents::TurnAgentResult turnResult = turnCoordinator.Execute(
        router,
        input,
        context.GetRecentMessages(),
        profile.bMemoryEnabled,
        currentTurn,
        stopToken);
    const responseOutput& output = turnResult.response;
    std::vector<latencySample> turnTimings = output.timings;
    turnTimings.push_back({"turn_total", ElapsedMilliseconds(turnStarted), true});
    appLogger.Timing("turn #" + std::to_string(currentTurn), turnTimings);

    result.succeeded = output.bSuccess;
    result.fromAssistant = true;
    result.text = output.response;
    result.reason = output.reason;
    result.wasStreamed = output.bWasStreamed;
    if (!output.bSuccess)
    {
        if (stopToken.stop_requested())
        {
            SetState(RuntimeState::Idle, "The response was stopped.");
        }
        else
        {
            appLogger.Warning(output.reason);
            SetState(RuntimeState::Error, output.reason);
        }
        Publish(RuntimeEventKind::AssistantMessage, result.text, currentTurn);
        return finish(std::move(result));
    }

    if (!output.response.empty())
    {
        SetState(RuntimeState::Responding, "Reply ready for turn #" + std::to_string(currentTurn) + ".");
        context.AddMessage("assistant", output.response);
        Publish(RuntimeEventKind::AssistantMessage, output.response, currentTurn);
    }

    if (turnResult.memoryQueued)
    {
        SetState(RuntimeState::Remembering, "Checking turn #" + std::to_string(currentTurn) + " for durable memory.");
    }
    else
    {
        SetState(RuntimeState::Idle);
    }
    return finish(std::move(result));
}

void ReviaSession::PollBackgroundEvents()
{
    if (const std::optional<AffectSnapshot> affect = affectController.Tick())
    {
        PublishAffect(*affect);
    }

    const std::vector<agents::MemoryAgentEvent> memoryEvents =
        turnCoordinator.DrainMemoryEvents();
    for (const agents::MemoryAgentEvent& event : memoryEvents)
    {
        const std::string timingScope = event.operation == "memory_backfill"
            ? "memory backfill"
            : "memory turn #" + std::to_string(event.turnId);
        appLogger.Timing(timingScope, event.decision.timings);

        if (event.operation == "memory_backfill")
        {
            if (!event.decision.bSuccess)
            {
                appLogger.Warning("Memory embedding backfill failed: " + event.decision.reason);
            }
            else if (!event.saveSucceeded)
            {
                appLogger.Warning("Failed to save a backfilled memory embedding.");
            }
            continue;
        }

        if (!event.decision.bSuccess)
        {
            appLogger.Warning("Automatic memory evaluation failed: " + event.decision.reason);
        }
        else if (event.decision.bShouldRemember && !event.saveSucceeded)
        {
            appLogger.Warning("Failed to save automatic memory.");
        }
        else if (event.wasAdded)
        {
            Publish(RuntimeEventKind::Memory, "Remembered: " + event.decision.summary, event.turnId);
        }
    }

    if (!memoryEvents.empty() && state.load() == RuntimeState::Remembering)
    {
        SetState(RuntimeState::Idle);
    }
}

void ReviaSession::RequestStop()
{
    speechService.StopSpeaking();
    speechRecognitionService.Cancel();
    std::stop_source source;
    {
        std::lock_guard lock(cancellationMutex);
        source = activeStopSource;
    }
    source.request_stop();
    Publish(RuntimeEventKind::Activity, "Stop requested.");
}

void ReviaSession::Stop()
{
    RequestStop();
    std::lock_guard operationLock(operationMutex);
    if (!started.load() && !llamaServerProcess.WasStartedByRevia() &&
        !embeddingServerProcess.WasStartedByRevia())
    {
        speechService.Shutdown();
        speechRecognitionService.Shutdown();
        state.store(RuntimeState::Offline);
        return;
    }

    const auto shutdownStarted = std::chrono::steady_clock::now();
    std::vector<latencySample> shutdownTimings;
    SetState(RuntimeState::Stopping, "Shutting down Revia.");
    appLogger.Log("Shutting down...");

    auto stageStarted = std::chrono::steady_clock::now();
    speechService.Shutdown();
    shutdownTimings.push_back({"speech_service_stop", ElapsedMilliseconds(stageStarted)});

    stageStarted = std::chrono::steady_clock::now();
    speechRecognitionService.Shutdown();
    shutdownTimings.push_back({"speech_recognition_stop", ElapsedMilliseconds(stageStarted)});

    stageStarted = std::chrono::steady_clock::now();
    turnCoordinator.Stop();
    shutdownTimings.push_back({"memory_agent_stop", ElapsedMilliseconds(stageStarted)});
    PollBackgroundEvents();

    if (settings.llm.bShutdownServerOnExit && llamaServerProcess.WasStartedByRevia())
    {
        stageStarted = std::chrono::steady_clock::now();
        llamaServerProcess.Stop();
        shutdownTimings.push_back({"llm_server_stop", ElapsedMilliseconds(stageStarted)});
    }
    if (settings.embedding.bShutdownServerOnExit && embeddingServerProcess.WasStartedByRevia())
    {
        stageStarted = std::chrono::steady_clock::now();
        embeddingServerProcess.Stop();
        shutdownTimings.push_back({"embedding_server_stop", ElapsedMilliseconds(stageStarted)});
    }

    shutdownTimings.push_back({"shutdown_total", ElapsedMilliseconds(shutdownStarted), true});
    appLogger.Timing("shutdown", shutdownTimings);
    appLogger.Log("Shutdown complete.");
    started.store(false);
    busy.store(false);
    state.store(RuntimeState::Offline);
    Publish(RuntimeEventKind::StateChanged, "Offline");
}

void ReviaSession::SetConfirmationHandler(ConfirmationHandler handler)
{
    std::lock_guard lock(confirmationMutex);
    confirmationHandler = std::move(handler);
}

RuntimeEventBus& ReviaSession::Events()
{
    return eventBus;
}

RuntimeState ReviaSession::State() const
{
    return state.load();
}

bool ReviaSession::IsStarted() const
{
    return started.load();
}

bool ReviaSession::IsBusy() const
{
    return busy.load();
}

bool ReviaSession::IsSpeechEnabled() const
{
    return speechService.IsEnabled();
}

void ReviaSession::SetSpeechEnabled(const bool enabled)
{
    speechService.SetEnabled(enabled);
}

bool ReviaSession::BeginListening()
{
    speechService.StopSpeaking();
    return speechRecognitionService.BeginRecording();
}

bool ReviaSession::EndListening()
{
    return speechRecognitionService.EndRecording();
}

SessionResult ReviaSession::AnalyzeScreen(const std::string& prompt)
{
    std::lock_guard operationLock(operationMutex);
    SessionResult result;
    result.fromAssistant = true;
    if (!started.load() || !llmAvailable || !settings.vision.bEnabled)
    {
        result.succeeded = false;
        result.text = "My local vision system is not available.";
        result.reason = "Vision is disabled or the multimodal model is offline.";
        return result;
    }
    busy.store(true);
    const std::stop_token stopToken = BeginOperation();
    const auto totalStarted = std::chrono::steady_clock::now();
    RuntimeEvent event;
    event.kind = RuntimeEventKind::ComponentStatus;
    event.state = RuntimeState::Thinking;
    event.component = "Vision";
    event.phase = "Capturing";
    event.message = "Capturing the virtual desktop after explicit approval.";
    eventBus.Publish(event);
    SetState(RuntimeState::Thinking, "Capturing the screen for local analysis.");

    std::filesystem::path mediaDirectory(settings.llm.mediaPath);
    if (mediaDirectory.is_relative())
    {
        mediaDirectory = std::filesystem::absolute(mediaDirectory);
    }
    const vision::CaptureResult capture = screenCaptureService.CaptureDesktop(mediaDirectory);
    if (!capture.succeeded)
    {
        result.succeeded = false;
        result.text = "I could not capture the screen.";
        result.reason = capture.reason;
        busy.store(false);
        SetState(RuntimeState::Error, result.reason);
        event.phase = "Error";
        event.message = result.reason;
        eventBus.Publish(std::move(event));
        return result;
    }

    event.phase = "Analyzing";
    event.message = "Analyzing a " + std::to_string(capture.width) + "x" +
        std::to_string(capture.height) + " capture locally.";
    event.elapsedMilliseconds = capture.elapsedMilliseconds;
    eventBus.Publish(event);
    const responseOutput output = router.AnalyzeImage(
        capture.path,
        prompt.empty()
            ? "Describe the visible screen accurately and briefly. Point out anything that needs attention."
            : prompt,
        settings.vision.maxResponseTokens,
        stopToken);
    std::error_code cleanupError;
    std::filesystem::remove(capture.path, cleanupError);

    std::vector<latencySample> timings = output.timings;
    timings.push_back({"vision_capture", capture.elapsedMilliseconds});
    timings.push_back({"vision_total", ElapsedMilliseconds(totalStarted), true});
    appLogger.Timing("vision", timings);
    result.succeeded = output.bSuccess;
    result.text = output.response;
    result.reason = output.reason;
    if (output.bSuccess)
    {
        context.AddMessage("user", "Please analyze the screen I explicitly shared.");
        context.AddMessage("assistant", output.response);
        const AffectSnapshot affect = affectController.ObserveTurn(
            "analyze this screen", output.response, true);
        PublishAffect(affect);
        speechService.Speak(output.response, affect);
        SetState(RuntimeState::Idle);
    }
    else
    {
        SetState(stopToken.stop_requested() ? RuntimeState::Idle : RuntimeState::Error, output.reason);
    }
    event.state = state.load();
    event.phase = output.bSuccess ? "Ready" : stopToken.stop_requested() ? "Stopped" : "Error";
    event.message = output.bSuccess ? "Local screen analysis completed." : output.reason;
    event.elapsedMilliseconds = ElapsedMilliseconds(totalStarted);
    eventBus.Publish(std::move(event));
    busy.store(false);
    return result;
}

std::string ReviaSession::DisplayName() const
{
    return profile.displayName.empty() ? "Revia" : profile.displayName;
}

std::string ReviaSession::Greeting() const
{
    return llmAvailable ? "Hi. I'm online." : "";
}

speech::VoiceStudioSnapshot ReviaSession::VoiceStudio() const
{
    std::lock_guard lock(voiceStudioMutex);
    return speechService.VoiceStudio();
}

speech::VoiceOperationResult ReviaSession::CreateVoicePreset(
    const std::string& name,
    const std::string& description,
    const std::string& referenceText,
    const std::string& language)
{
    std::lock_guard lock(voiceStudioMutex);
    const auto startedAt = std::chrono::steady_clock::now();
    Publish(RuntimeEventKind::Activity,
        "Voice Studio is creating a reusable Qwen3-TTS reference. The first run downloads model weights.");
    speech::VoiceOperationResult result = speechService.CreateVoicePreset(
        name, description, referenceText, language);
    appLogger.Timing("voice design", {
        {"qwen_voice_design", result.elapsedMilliseconds},
        {"voice_design_total", ElapsedMilliseconds(startedAt), true}
    });
    if (!result.succeeded)
    {
        appLogger.Warning("Voice design failed: " + result.message);
    }
    return result;
}

speech::VoiceOperationResult ReviaSession::PreviewVoice(
    const std::string& presetId,
    const std::string& text)
{
    std::lock_guard lock(voiceStudioMutex);
    const auto startedAt = std::chrono::steady_clock::now();
    speech::VoiceOperationResult result = speechService.PreviewVoice(presetId, text);
    appLogger.Timing("voice preview", {
        {"qwen_voice_clone", result.elapsedMilliseconds},
        {"voice_preview_total", ElapsedMilliseconds(startedAt), true}
    });
    if (!result.succeeded)
    {
        appLogger.Warning("Voice preview failed: " + result.message);
    }
    return result;
}

speech::VoiceOperationResult ReviaSession::AssignVoice(
    const std::string& profileId,
    const std::string& presetId)
{
    std::lock_guard lock(voiceStudioMutex);
    speech::VoiceOperationResult result = speechService.AssignVoice(profileId, presetId);
    if (result.succeeded)
    {
        appLogger.Log(result.message);
    }
    else
    {
        appLogger.Warning("Voice assignment failed: " + result.message);
    }
    return result;
}

bool ReviaSession::EnsureLLMAvailable(const std::stop_token stopToken)
{
    const healthOutput initialHealth = router.CheckLLMHealth();
    if (initialHealth.bIsAvailable)
    {
        appLogger.Log("LLM backend is available." + BackendCapacity(initialHealth));
        return true;
    }
    if (initialHealth.status == systemStatus::Yellow)
    {
        appLogger.Warning(initialHealth.reason);
        return false;
    }
    if (settings.llm.backend != "LLamaCpp" || !settings.llm.bAutoStartServer)
    {
        appLogger.Warning("The configured LLM backend is unavailable and was not started.");
        return false;
    }

    appLogger.Log("LLM backend is offline. Starting llama.cpp...");
    std::string launchError;
    if (!llamaServerProcess.Start(settings.llm, launchError))
    {
        appLogger.Warning(launchError);
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(settings.llm.startupTimeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline && !stopToken.stop_requested())
    {
        if (!llamaServerProcess.IsRunning())
        {
            appLogger.Warning("llama.cpp exited before becoming ready.");
            llamaServerProcess.Stop();
            return false;
        }
        const healthOutput readyHealth = router.CheckLLMHealth();
        if (readyHealth.bIsAvailable)
        {
            appLogger.Log(
                "llama.cpp started with automatic hardware fitting." +
                BackendCapacity(readyHealth));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    appLogger.Warning(stopToken.stop_requested()
        ? "Stopped while waiting for llama.cpp."
        : "Timed out waiting for llama.cpp to become ready.");
    llamaServerProcess.Stop();
    return false;
}

bool ReviaSession::EnsureEmbeddingAvailable(const std::stop_token stopToken)
{
    if (!settings.embedding.bEnabled)
    {
        appLogger.Log("Semantic memory is disabled; SQLite FTS retrieval remains available.");
        return false;
    }

    const healthOutput initialHealth = router.CheckEmbeddingHealth();
    if (initialHealth.bIsAvailable)
    {
        appLogger.Log("Semantic-memory embeddings are available.");
        return true;
    }
    if (initialHealth.status == systemStatus::Yellow || !settings.embedding.bAutoStartServer)
    {
        appLogger.Warning(initialHealth.reason + " Falling back to SQLite FTS retrieval.");
        return false;
    }

    appLogger.Log("Embedding backend is offline. Starting dedicated llama.cpp embeddings...");
    std::string launchError;
    if (!embeddingServerProcess.StartEmbedding(settings.embedding, launchError))
    {
        appLogger.Warning(launchError + " Falling back to SQLite FTS retrieval.");
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(settings.embedding.startupTimeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline && !stopToken.stop_requested())
    {
        if (!embeddingServerProcess.IsRunning())
        {
            appLogger.Warning("The embedding server exited before becoming ready.");
            embeddingServerProcess.Stop();
            return false;
        }
        if (router.CheckEmbeddingHealth().bIsAvailable)
        {
            appLogger.Log("Dedicated semantic-memory embeddings are available.");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    appLogger.Warning(stopToken.stop_requested()
        ? "Stopped while waiting for the embedding server."
        : "Timed out waiting for the embedding server; SQLite FTS fallback remains active.");
    embeddingServerProcess.Stop();
    return false;
}

bool ReviaSession::TryHandleActionInput(const std::string& input, SessionResult& result)
{
    if (input == "/capabilities")
    {
        result.text = actionRuntime.StatusJson();
        SetState(RuntimeState::Idle);
        return true;
    }

    if (input.rfind("/plan ", 0) == 0)
    {
        if (!actionRuntime.IsInitialized())
        {
            result.succeeded = false;
            result.text = "Action runtime is not initialized.";
            SetState(RuntimeState::Blocked, result.text);
            return true;
        }

        SetState(RuntimeState::Thinking, "Planning a constrained action.");
        const responseOutput proposal = router.PlanAction(input.substr(6));
        if (!proposal.bSuccess)
        {
            result.succeeded = false;
            result.text = proposal.response;
            result.reason = proposal.reason;
            SetState(RuntimeState::Error, result.reason);
            return true;
        }

        auto parsed = actionRuntime.ParseJson(proposal.response);
        if (!parsed.succeeded)
        {
            result.succeeded = false;
            result.text = "Action proposal rejected: " + parsed.error;
            result.reason = parsed.error;
            SetState(RuntimeState::Blocked, result.reason);
            return true;
        }
        parsed.request.requestedBy = "llm";
        result = ExecuteAction(std::move(parsed.request));
        return true;
    }

    auto parsed = actionRuntime.ParseCommand(input);
    if (!parsed.recognized)
    {
        return false;
    }
    if (!parsed.succeeded)
    {
        result.succeeded = false;
        result.text = "Action command rejected: " + parsed.error;
        result.reason = parsed.error;
        SetState(RuntimeState::Blocked, result.reason);
        return true;
    }

    result = ExecuteAction(std::move(parsed.request));
    return true;
}

SessionResult ReviaSession::ExecuteAction(actions::ActionRequest request)
{
    SessionResult result;
    if (!actionRuntime.IsInitialized())
    {
        result.succeeded = false;
        result.text = "Action runtime is not initialized.";
        SetState(RuntimeState::Blocked, result.text);
        return result;
    }

    const actions::PolicyDecision decision = actionRuntime.Evaluate(request);
    bool confirmed = false;
    if (decision.verdict == actions::PolicyVerdict::RequiresConfirmation)
    {
        SetState(RuntimeState::WaitingForConfirmation, decision.reason);
        ConfirmationHandler handler;
        {
            std::lock_guard lock(confirmationMutex);
            handler = confirmationHandler;
        }
        confirmed = handler && handler(request, decision);
    }

    const auto actionStarted = std::chrono::steady_clock::now();
    RuntimeEvent startedEvent;
    startedEvent.kind = RuntimeEventKind::ComponentStatus;
    startedEvent.state = RuntimeState::Acting;
    startedEvent.message = "Executing " + actions::ToString(request.type) + ".";
    startedEvent.component = "Automation";
    startedEvent.phase = "Running";
    eventBus.Publish(std::move(startedEvent));
    SetState(RuntimeState::Acting, "Executing " + actions::ToString(request.type) + ".");
    const actions::ActionOutcome outcome = actionRuntime.Execute(request, confirmed);
    RuntimeEvent completedEvent;
    completedEvent.kind = RuntimeEventKind::ComponentStatus;
    completedEvent.state = RuntimeState::Acting;
    completedEvent.message = outcome.result.message;
    completedEvent.component = "Automation";
    completedEvent.phase = outcome.result.succeeded ? "Ready" : "Blocked";
    completedEvent.elapsedMilliseconds = ElapsedMilliseconds(actionStarted);
    eventBus.Publish(std::move(completedEvent));
    result.succeeded = outcome.result.succeeded;
    result.text = FormatActionOutcome(outcome);
    if (!result.succeeded)
    {
        result.reason = outcome.result.message;
    }

    if (outcome.policy.verdict == actions::PolicyVerdict::Blocked ||
        (outcome.policy.verdict == actions::PolicyVerdict::RequiresConfirmation && !confirmed))
    {
        SetState(RuntimeState::Blocked, outcome.result.message);
    }
    else
    {
        SetState(RuntimeState::Idle, outcome.result.message);
    }
    return result;
}

std::string ReviaSession::FormatActionOutcome(const actions::ActionOutcome& outcome)
{
    std::ostringstream stream;
    stream << (outcome.result.succeeded ? "Action succeeded: " : "Action stopped: ")
           << outcome.result.message << '\n';
    for (const std::string& entry : outcome.result.entries)
    {
        stream << "  " << entry << '\n';
    }
    if (!outcome.result.content.empty())
    {
        stream << "----- file content -----\n"
               << outcome.result.content
               << "\n----- end content -----\n";
    }
    return stream.str();
}

std::stop_token ReviaSession::BeginOperation()
{
    std::lock_guard lock(cancellationMutex);
    activeStopSource = std::stop_source{};
    return activeStopSource.get_token();
}

void ReviaSession::SetState(const RuntimeState newState, const std::string& activity)
{
    state.store(newState);
    Publish(
        RuntimeEventKind::StateChanged,
        activity.empty() ? ToString(newState) : activity);
}

void ReviaSession::PublishAffect(const AffectSnapshot& affect)
{
    RuntimeEvent event;
    event.kind = RuntimeEventKind::AffectChanged;
    event.state = state.load();
    event.message = affect.reason;
    event.affect = affect.state;
    event.affectIntensity = affect.intensity;
    eventBus.Publish(std::move(event));
}

void ReviaSession::Publish(
    const RuntimeEventKind kind,
    const std::string& message,
    const std::uint64_t turnId) const
{
    eventBus.Publish(RuntimeEvent{kind, state.load(), message, turnId});
}

} // namespace revia::runtime
