#include "Runtime/reviaSession.h"
#include "Core/localApiKey.h"
#include "Agents/replyFragmenter.h"
#include "Memory/longTermMemory.h"
#include "Planning/goalPlanner.h"

#include <algorithm>
#include <cctype>
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

    std::string ToLowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    }

    std::string Trim(const std::string& value)
    {
        const std::size_t first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return {};
        }
        return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
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
    : goalRunner(actionRuntime, goalStore)
{
    appLogger.SetSink([this](const std::string& line)
    {
        Publish(RuntimeEventKind::Activity, line);
    });
    goalRunner.SetProgressHandler([this](const goals::GoalProgress& progress)
    {
        PublishGoalProgress(progress);
    });
    // A goal step that needs confirmation asks the same handler an interactive action
    // does. Without this the runner would see no handler and treat every confirmable
    // step as refused, which reads as a policy block rather than a missing prompt.
    goalRunner.SetConfirmationHandler([this](
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision)
    {
        ConfirmationHandler handler;
        {
            std::lock_guard lock(confirmationMutex);
            handler = confirmationHandler;
        }
        if (!handler)
        {
            return false;
        }
        SetState(RuntimeState::WaitingForConfirmation, decision.reason);
        return handler(request, decision);
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

    // Goals are reported, never auto-resumed. Restarting into unattended execution of
    // work the user has not re-approved belongs to Stage 5, behind its own job contract.
    stageStarted = std::chrono::steady_clock::now();
    const std::vector<goals::Goal> resumable = goalStore.LoadResumable();
    if (!resumable.empty())
    {
        std::ostringstream resumeNotice;
        resumeNotice << resumable.size()
            << (resumable.size() == 1 ? " goal was" : " goals were")
            << " left unfinished. Use /goals to list them, /goals resume <id> to continue:";
        for (const goals::Goal& goal : resumable)
        {
            resumeNotice << "\n  " << goal.id << "  " << goal.title;
        }
        appLogger.Log(resumeNotice.str());
    }
    startupTimings.push_back({"goal_store_scan", ElapsedMilliseconds(stageStarted)});

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
        // Reuses turnId as the correlation field rather than adding a parallel one; the
        // shell only ever needs to match a reply to the audio for it.
        event.turnId = speechEvent.utteranceId;
        eventBus.Publish(std::move(event));
    });
    // Barge-in arms the microphone only while Revia is speaking, and hands the floor back
    // by starting a capture so the interruption is actually heard rather than just
    // silencing the reply.
    speechService.ConfigureBargeIn(settings.bargeIn, settings.speechRecognition.sampleRate);
    speechService.SetBargeInHandler([this]()
    {
        if (settings.speechRecognition.bEnabled)
        {
            speechRecognitionService.BeginRecording();
        }
    });
    startupTimings.push_back({"speech_service_init", ElapsedMilliseconds(stageStarted)});

    // Loading the Qwen3-TTS voice used to run here, before llama.cpp, purely so llama.cpp
    // would fit around VRAM the voice model already held. That put 20-70 seconds of model
    // load on the path to the first message even though nothing about chat needs a voice.
    // Reserving the voice's VRAM budget in llama.cpp's fit target buys the same guarantee,
    // so the load can happen in the background once the chat path is up.
    const bool deferredVoiceLoad = speechService.HasActiveQwenVoice();
    if (deferredVoiceLoad)
    {
        settings.llm.reservedVramMiB = settings.speech.qwenMinimumFreeVramMiB;
        appLogger.Log(
            "Reserving " + std::to_string(settings.speech.qwenMinimumFreeVramMiB) +
            " MiB of VRAM for the assigned Qwen3-TTS voice; it loads in the background "
            "once chat is available.");
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
    windowEventMonitor.Start(
        settings.perception,
        [this](const perception::WindowObservation& observation)
        {
            // Retained in memory only. Excluded windows never reach this handler, so they
            // cannot enter the history either -- the filter is the single gate.
            activityHistory.Record(observation);

            // Structured facts only, and only ones that cleared the filter. The activity
            // feed is the visible record of what perception noticed, which is what makes
            // the capability auditable rather than merely configurable.
            RuntimeEvent event;
            event.kind = RuntimeEventKind::ComponentStatus;
            event.state = state.load();
            event.component = "Perception";
            event.phase = perception::ToString(observation.kind);
            event.message = observation.application +
                (observation.windowTitle.empty()
                    ? std::string()
                    : " - " + observation.windowTitle);
            eventBus.Publish(std::move(event));
        },
        [this](const std::string& phase, const std::string& detail)
        {
            RuntimeEvent event;
            event.kind = RuntimeEventKind::ComponentStatus;
            event.state = state.load();
            event.component = "Perception";
            event.phase = phase;
            event.message = detail;
            eventBus.Publish(std::move(event));
        });
    startupTimings.push_back({"perception_init", ElapsedMilliseconds(stageStarted)});

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
    if (deferredVoiceLoad)
    {
        StartVoiceWarmup();
    }
    StartInitiativeLoop();
    if (settings.speech.bEnabled && settings.speech.bSpeakGreeting && !Greeting().empty())
    {
        speechService.Speak(Greeting(), affectController.Current());
    }
    return true;
}

void ReviaSession::StartInitiativeLoop()
{
    StopInitiativeLoop();
    if (!settings.initiative.bEnabled)
    {
        return;
    }
    initiativeController.Configure(settings.initiative);
    initiativeWorker = std::jthread([this](const std::stop_token stopToken)
    {
        while (!stopToken.stop_requested())
        {
            // Slow on purpose. Nothing here is time critical, and a companion that
            // evaluates whether to speak several times a second is one bug away from
            // being unbearable.
            for (int slice = 0; slice < 120 && !stopToken.stop_requested(); ++slice)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if (stopToken.stop_requested() || !started.load() || busy.load())
            {
                continue;
            }
            // Never interrupt while Revia is already talking or listening.
            if (speechRecognitionService.IsRecording())
            {
                continue;
            }

            initiative::AttentionContext context = initiative::SampleDesktop();
            initiative::InitiativeController::Evidence evidence;
            evidence.recentActivity = activityHistory.Spans(std::chrono::minutes{90});
            // A goal an earlier run left unfinished is the strongest thing Revia knows:
            // the user asked for it, and it is still incomplete.
            evidence.unfinishedGoals = goalStore.LoadResumable();
            const auto consideration = initiativeController.Consider(evidence, context);
            if (!consideration.hasProposal)
            {
                continue;
            }

            RuntimeEvent event;
            event.kind = RuntimeEventKind::Proposal;
            event.state = state.load();
            event.component = "Initiative";
            event.phase = consideration.proposal.id;
            event.message = consideration.proposal.message;
            event.detail = consideration.proposal.evidence;
            eventBus.Publish(std::move(event));

            appLogger.Log("Proposal offered: " + consideration.proposal.evidence);
            if (speechService.IsEnabled())
            {
                speechService.Speak(
                    consideration.proposal.message, affectController.Current());
            }
        }
    });
}

void ReviaSession::StopInitiativeLoop()
{
    if (initiativeWorker.joinable())
    {
        initiativeWorker.request_stop();
        initiativeWorker.join();
    }
}

std::string ReviaSession::InitiativeStatus() const
{
    return initiativeController.Status();
}

std::vector<initiative::Proposal> ReviaSession::PendingProposals() const
{
    return initiativeController.Pending();
}

SessionResult ReviaSession::AcceptProposal(const std::string& proposalId)
{
    SessionResult result;
    std::string goalRequest;
    std::string resumeGoalId;
    for (const initiative::Proposal& proposal : initiativeController.Pending())
    {
        if (proposal.id == proposalId)
        {
            goalRequest = proposal.goalRequest;
            resumeGoalId = proposal.resumeGoalId;
            break;
        }
    }
    initiativeController.Accept(proposalId);
    result.text = "Noted.";
    if (!resumeGoalId.empty())
    {
        // Straight to the runner, which re-verifies every remaining step. Accepting a
        // proposal is a shortcut for typing the command, never a way around it.
        const goals::Goal finished = ResumeGoalUnlocked(resumeGoalId);
        result.succeeded = finished.status == goals::GoalStatus::Succeeded;
        result.text = FormatGoalSummary(finished);
        if (!result.succeeded)
        {
            result.reason = goals::ToString(finished.stopReason);
        }
        return result;
    }
    // A proposal that names a goal hands it to the runner, which rehearses, confirms,
    // budgets, and audits exactly as it would for a typed request. Accepting adds no
    // authority; it only saves the typing.
    if (!goalRequest.empty())
    {
        return TryHandleGoalInput("/goal " + goalRequest, result) ? result : result;
    }
    return result;
}

std::vector<learning::Lesson> ReviaSession::DrawLessons() const
{
    return learning::LearningReview::Draw(
        goalStore.LoadRecent(50), initiativeController.Counters());
}

bool ReviaSession::ApproveLesson(const std::string& lessonId, std::string& outSummary)
{
    for (const learning::Lesson& lesson : DrawLessons())
    {
        if (lesson.id != lessonId)
        {
            continue;
        }
        // Written through the ordinary memory path as an ordinary preference. A lesson is
        // a sentence to remember, not a policy: nothing here can widen a capability,
        // change a budget, or alter how an action is authorised.
        memoryDecision decision;
        decision.bSuccess = true;
        decision.bShouldRemember = true;
        decision.category = learning::LearningReview::MemoryCategory(lesson);
        decision.summary = learning::LearningReview::MemorySummary(lesson);
        decision.reason = "Reviewed and approved by the user.";

        bool added = false;
        longTermMemory memory;
        if (!memory.Save(decision, added))
        {
            outSummary = "The lesson could not be saved to memory.";
            return false;
        }
        outSummary = added
            ? "Remembered: " + decision.summary
            : "Already remembered something equivalent.";
        appLogger.Log("Approved lesson " + lesson.id + ": " + decision.summary);
        return true;
    }
    outSummary = "No lesson with that id is currently on offer.";
    return false;
}

void ReviaSession::DismissProposal(const std::string& proposalId)
{
    initiativeController.Dismiss(proposalId, std::chrono::system_clock::now());
    appLogger.Log("Proposal dismissed. Revia will wait longer before offering again.");
}

void ReviaSession::StartVoiceWarmup()
{
    StopVoiceWarmup();
    voiceWarmupFinished.store(false);
    voiceWarmupWorker = std::jthread([this](const std::stop_token stopToken)
    {
        struct FinishedGuard
        {
            std::atomic<bool>& flag;
            ~FinishedGuard() { flag.store(true); }
        } finishedGuard{voiceWarmupFinished};

        // Stopping a reply cancels the active Qwen request by killing the worker process,
        // which also kills a load still in flight. That could not happen while the load
        // was part of startup; now that it runs alongside chat, it has to survive a Stop.
        constexpr int MaximumAttempts = 3;
        const auto startedAt = std::chrono::steady_clock::now();
        for (int attempt = 1; attempt <= MaximumAttempts; ++attempt)
        {
            if (stopToken.stop_requested() || !started.load())
            {
                return;
            }
            // PrepareActiveVoice publishes its own Loading/Ready voice component events,
            // so the shell shows the voice arriving without startup having waited for it.
            const speech::VoiceOperationResult prepared = speechService.PrepareActiveVoice();
            if (prepared.succeeded)
            {
                appLogger.Timing(
                    "voice_warmup",
                    {{"qwen_voice_model_load", ElapsedMilliseconds(startedAt), true}});
                appLogger.Log("Background voice load finished: " + prepared.message);
                return;
            }
            if (stopToken.stop_requested() || !started.load())
            {
                return;
            }
            if (attempt == MaximumAttempts)
            {
                appLogger.Warning(
                    "Assigned Qwen voice could not load after " +
                    std::to_string(MaximumAttempts) + " attempts: " + prepared.message +
                    " Windows SAPI remains available.");
                return;
            }
            appLogger.Log(
                "Voice load attempt " + std::to_string(attempt) + " did not finish (" +
                prepared.message + "); retrying in the background.");
            for (int waited = 0; waited < 8 && !stopToken.stop_requested(); ++waited)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }
    });
}

void ReviaSession::StopVoiceWarmup()
{
    if (!voiceWarmupWorker.joinable())
    {
        return;
    }
    voiceWarmupWorker.request_stop();
    // The load sits inside a blocking HTTP call to the Qwen worker, so requesting a stop
    // is not enough on its own; killing that worker is what makes the call return. Repeat
    // it while waiting, because an attempt that was already past its own cancellation
    // check can spawn a fresh worker after the first kill. Bounded so a worker that
    // refuses to die delays shutdown by seconds rather than a whole model load.
    constexpr int MaximumWaitSlices = 40;
    for (int slice = 0; slice < MaximumWaitSlices && !voiceWarmupFinished.load(); ++slice)
    {
        speechService.StopSpeaking();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!voiceWarmupFinished.load())
    {
        appLogger.Warning("The background voice load did not stop; waiting for it to finish.");
    }
    voiceWarmupWorker.join();
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
    // Counts sentences already spoken from the stream, so the completed reply is not
    // said a second time at the end of the turn.
    std::uint64_t streamedUtterances = 0;
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
                if (streamedUtterances > 0)
                {
                    // Already said, sentence by sentence, while it was being generated.
                    // The shell has been showing those fragments as they were spoken, so
                    // there is nothing left to say or to reveal.
                    finished.spokenAsFragments = true;
                }
                else if (speechService.IsEnabled() && ShouldSpeakOnCurrentChannel())
                {
                    // Only claim the text is pending on speech when speech is actually
                    // going to happen. Anything else would hold a reply the user is never
                    // going to hear, which is worse than text arriving early.
                    finished.utteranceId = ++utteranceCounter;
                    finished.speechPending = true;
                    speechService.Speak(finished.text, affect, finished.utteranceId);
                }
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
    // Revia's own posture reaches the model, not just the status chip and the speech rate.
    // Phrased as her stance rather than a claim about the user: the affect controller
    // describes how she is approaching this turn and never infers what the user feels.
    {
        const AffectSnapshot posture = affectController.Current();
        std::ostringstream postureLine;
        postureLine << "Your current response posture is "
            << ToString(posture.state) << " at "
            << static_cast<int>(posture.intensity * 100.0F) << "% intensity, because "
            << posture.reason
            << " Let it colour your tone and pacing. Do not name it, do not describe your "
               "own feelings, and do not assume anything about how the user feels.";
        router.SetPosture(postureLine.str());
    }
    const std::uint64_t currentTurn = ++turnCounter;
    const auto turnStarted = std::chrono::steady_clock::now();
    SetState(RuntimeState::Thinking, "Thinking about turn #" + std::to_string(currentTurn) + ".");

    // Speak each sentence as soon as it is complete, rather than waiting for the whole
    // reply. The first sentence usually exists long before the last one, so this cuts the
    // wait before Revia starts talking from the length of a paragraph to the length of a
    // sentence. Only worth doing when there is a voice to speak it.
    const bool streamSpeech = speechService.IsEnabled() && ShouldSpeakOnCurrentChannel();
    agents::ReplyFragmenter fragmenter;
    std::string streamedText;
    const auto emitFragment = [&](const std::string& fragment)
    {
        const std::uint64_t utteranceId = ++utteranceCounter;
        ++streamedUtterances;
        speechService.Speak(fragment, affectController.Current(), utteranceId);
        RuntimeEvent partial;
        partial.kind = RuntimeEventKind::ReplyFragment;
        partial.state = RuntimeState::Responding;
        partial.message = fragment;
        partial.turnId = utteranceId;
        eventBus.Publish(std::move(partial));
    };

    messageRouter::DeltaHandler onDelta;
    if (streamSpeech)
    {
        onDelta = [&](const std::string& delta)
        {
            streamedText += delta;
            for (const std::string& fragment : fragmenter.Consume(delta))
            {
                emitFragment(fragment);
            }
        };
    }

    const agents::TurnAgentResult turnResult = turnCoordinator.Execute(
        router,
        input,
        context.GetRecentMessages(),
        profile.bMemoryEnabled,
        currentTurn,
        stopToken,
        onDelta);
    if (streamSpeech && turnResult.response.bSuccess)
    {
        // Safety net, not an optimisation. The deltas are supposed to sum to the finished
        // reply, but the generator holds characters back to avoid emitting a partial
        // special token, and anything it fails to release would end a sentence mid-word.
        // The finished response is the source of truth, so recover any shortfall from it
        // rather than trusting the stream to have been complete.
        const std::string& complete = turnResult.response.response;
        if (complete.size() > streamedText.size() &&
            complete.compare(0, streamedText.size(), streamedText) == 0)
        {
            for (const std::string& fragment :
                fragmenter.Consume(complete.substr(streamedText.size())))
            {
                emitFragment(fragment);
            }
        }

        // Whatever did not end on a sentence boundary still has to be said.
        const std::string remainder = fragmenter.Flush();
        if (!remainder.empty())
        {
            emitFragment(remainder);
        }
    }
    const responseOutput& output = turnResult.response;
    {
        const AffectSnapshot posture = affectController.Current();
        std::ostringstream trace;
        trace << "Posture: " << ToString(posture.state) << " at "
            << static_cast<int>(posture.intensity * 100.0F) << "% - " << posture.reason;
        if (!output.reasoning.empty())
        {
            trace << "\n\nReasoning:\n" << output.reasoning;
        }
        if (streamedUtterances > 0)
        {
            trace << "\n\nSpoken in " << streamedUtterances
                << (streamedUtterances == 1 ? " fragment" : " fragments")
                << " as it was generated.";
        }
        if (!output.timings.empty())
        {
            trace << "\n\nTiming:";
            for (const latencySample& sample : output.timings)
            {
                trace << "\n  " << sample.stage << " " << sample.milliseconds << "ms";
            }
        }
        result.reasoning = trace.str();
    }
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
    // Order matters. The warmup is built to survive RequestStop, because stopping a reply
    // must not cost the session its voice. Shutdown is the one case where it must not
    // retry, so cancel it first, then let RequestStop kill the Qwen worker so an in-flight
    // load fails fast and the join returns instead of waiting out a model load.
    if (voiceWarmupWorker.joinable())
    {
        voiceWarmupWorker.request_stop();
    }
    RequestStop();
    // Must precede speechService.Shutdown() below, which both workers still call into.
    StopInitiativeLoop();
    StopVoiceWarmup();
    std::lock_guard operationLock(operationMutex);
    if (!started.load() && !llamaServerProcess.WasStartedByRevia() &&
        !embeddingServerProcess.WasStartedByRevia())
    {
        speechService.Shutdown();
        speechRecognitionService.Shutdown();
        // This branch must unhook too. A system-wide event hook that is not removed
        // outlives the process that installed it.
        windowEventMonitor.Shutdown();
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

    // Before the rest: a system-wide event hook outlives the process that installed it if
    // it is not unhooked, so this is not a step to leave until after something can fail.
    stageStarted = std::chrono::steady_clock::now();
    if (settings.perception.bEnabled)
    {
        // Counts only. How much was observed is worth recording for transparency; what
        // was observed is not written to a plaintext log on disk.
        const perception::PerceptionCounters counters = windowEventMonitor.Counters();
        appLogger.Log(
            "Perception this session: observed " + std::to_string(counters.observed) +
            ", excluded " + std::to_string(counters.excluded) +
            ", coalesced " + std::to_string(counters.coalesced) +
            ", rate limited " + std::to_string(counters.rateLimited) + ".");
    }
    windowEventMonitor.Shutdown();
    shutdownTimings.push_back({"perception_stop", ElapsedMilliseconds(stageStarted)});

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

bool ReviaSession::ShouldSpeakOnCurrentChannel() const
{
    std::lock_guard lock(channelMutex);
    if (outputTarget == outputChannel::LocalVoice)
    {
        return true;
    }
    // Composing into somebody else's application. Reading Discord messages aloud as they
    // are typed is noise, so speech is off unless this executable was explicitly opted in.
    const std::string lowered = ToLowerCopy(outputApplication);
    for (const std::string& allowed : settings.channels.voiceEnabledApplications)
    {
        if (ToLowerCopy(allowed) == lowered)
        {
            return true;
        }
    }
    return false;
}

void ReviaSession::SetOutputChannel(
    const outputChannel channel,
    const std::string& applicationName)
{
    {
        std::lock_guard lock(channelMutex);
        outputTarget = channel;
        outputApplication = applicationName;
    }
    RuntimeEvent event;
    event.kind = RuntimeEventKind::ComponentStatus;
    event.state = state.load();
    event.component = "Channel";
    event.phase = channel == outputChannel::LocalVoice ? "Voice" : "TextOnly";
    event.message = channel == outputChannel::LocalVoice
        ? std::string("Talking here, with voice.")
        : "Composing into " + (applicationName.empty() ? "another application"
            : applicationName) + "; replies are text only.";
    eventBus.Publish(std::move(event));
}

std::string ReviaSession::OutputChannelStatus() const
{
    std::lock_guard lock(channelMutex);
    if (outputTarget == outputChannel::LocalVoice)
    {
        return "Talking here. Replies are spoken when voice is enabled.";
    }
    return "Composing into " +
        (outputApplication.empty() ? "another application" : outputApplication) +
        ". Replies are text only and are not read aloud.";
}

bool ReviaSession::IsPerceptionEnabled() const
{
    return settings.perception.bEnabled;
}

bool ReviaSession::IsPerceptionPaused() const
{
    return windowEventMonitor.IsPaused();
}

void ReviaSession::SetPerceptionPaused(const bool paused)
{
    windowEventMonitor.SetPaused(paused);
}

perception::PerceptionCounters ReviaSession::PerceptionCounters() const
{
    return windowEventMonitor.Counters();
}

std::string ReviaSession::PerceptionStatus() const
{
    std::ostringstream stream;
    if (!settings.perception.bEnabled)
    {
        stream << "Ambient perception is OFF. Nothing about your windows is observed.\n"
            << "Enable it in Config/settings.json under \"perception\".";
        return stream.str();
    }
    const perception::PerceptionCounters counters = windowEventMonitor.Counters();
    stream << "Ambient perception is "
        << (windowEventMonitor.IsPaused() ? "PAUSED" : "WATCHING")
        << ". Window and focus events only - no screen capture, no model.\n"
        << "Observed " << counters.observed << ", excluded " << counters.excluded
        << ", coalesced " << counters.coalesced
        << ", rate limited " << counters.rateLimited << ".\n"
        << settings.perception.excludedApplications.size()
        << " excluded applications and "
        << settings.perception.excludedTitleFragments.size()
        << " excluded title fragments are in effect.\n"
        << "Retained in memory only: " << activityHistory.Size()
        << " activity spans, discarded when Revia stops.\n"
        << "Use /perception pause, resume, history [minutes], or forget.";
    return stream.str();
}

std::string ReviaSession::RecentActivity(const std::chrono::minutes window) const
{
    if (!settings.perception.bEnabled)
    {
        return "Ambient perception is off, so nothing has been observed.";
    }
    return activityHistory.Summarize(window);
}

void ReviaSession::ForgetActivity()
{
    activityHistory.Clear();
    appLogger.Log("Observation history cleared at the user's request.");
}

void ReviaSession::SetBargeInEnabled(const bool enabled)
{
    speechService.SetBargeInEnabled(enabled);
    appLogger.Log(enabled
        ? "Barge-in enabled: speaking over Revia will stop her."
        : "Barge-in disabled: Revia will finish what she is saying.");
}

bool ReviaSession::IsBargeInEnabled() const
{
    return speechService.IsBargeInEnabled();
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

goals::Goal ReviaSession::RunGoal(goals::Goal goal)
{
    std::lock_guard operationLock(operationMutex);
    // Entry from outside a turn, so this run owns the cancellation scope. The command
    // path reaches the Unlocked variants directly and reuses Submit's scope instead.
    BeginOperation();
    return RunGoalUnlocked(std::move(goal));
}

goals::Goal ReviaSession::ResumeGoal(const std::string& goalId)
{
    std::lock_guard operationLock(operationMutex);
    BeginOperation();
    return ResumeGoalUnlocked(goalId);
}

std::vector<goals::Goal> ReviaSession::RecentGoals(const std::size_t maxGoals) const
{
    return goalStore.LoadRecent(maxGoals);
}

std::vector<goals::Goal> ReviaSession::ResumableGoals() const
{
    return goalStore.LoadResumable();
}

goals::Goal ReviaSession::RunGoalUnlocked(goals::Goal goal)
{
    if (!actionRuntime.IsInitialized())
    {
        goal.status = goals::GoalStatus::Failed;
        goal.stopReason = goals::StopReason::PolicyBlocked;
        SetState(RuntimeState::Blocked, "Action runtime is not initialized.");
        return goal;
    }
    // Validate before announcing anything, so an unverifiable plan is rejected without
    // ever entering the Acting state.
    std::string planError;
    if (!goals::GoalRunner::Validate(goal, planError))
    {
        goal.status = goals::GoalStatus::Failed;
        goal.stopReason = goals::StopReason::InvalidPlan;
        appLogger.Warning("Goal plan rejected: " + planError);
        SetState(RuntimeState::Blocked, "Goal plan rejected: " + planError);
        return goal;
    }

    busy.store(true);
    const std::stop_token stopToken = CurrentOperationToken();
    const auto startedAt = std::chrono::steady_clock::now();
    SetState(RuntimeState::Acting, "Running goal: " + goal.title);
    return FinishGoalRun(goalRunner.Run(std::move(goal), stopToken), startedAt);
}

goals::Goal ReviaSession::ResumeGoalUnlocked(const std::string& goalId)
{
    goals::Goal goal;
    goal.id = goalId;
    if (!actionRuntime.IsInitialized())
    {
        goal.status = goals::GoalStatus::Failed;
        goal.stopReason = goals::StopReason::PolicyBlocked;
        SetState(RuntimeState::Blocked, "Action runtime is not initialized.");
        return goal;
    }

    busy.store(true);
    const std::stop_token stopToken = CurrentOperationToken();
    const auto startedAt = std::chrono::steady_clock::now();
    SetState(RuntimeState::Acting, "Resuming goal " + goalId + ".");
    return FinishGoalRun(goalRunner.Resume(goalId, stopToken), startedAt);
}

goals::Goal ReviaSession::FinishGoalRun(
    goals::Goal finished,
    const std::chrono::steady_clock::time_point startedAt)
{
    appLogger.Timing("goal", {
        {"goal_actions", static_cast<double>(finished.spend.actions)},
        {"goal_retries", static_cast<double>(finished.spend.retries)},
        {"goal_run", ElapsedMilliseconds(startedAt), true}});
    busy.store(false);

    const std::string summary = FormatGoalSummary(finished);
    if (finished.status == goals::GoalStatus::Succeeded)
    {
        appLogger.Log(summary);
        SetState(RuntimeState::Idle, summary);
    }
    else
    {
        // Anything other than Succeeded is reported, never retried automatically. A goal
        // that exhausted its budget stopping quietly is the failure mode Stage 4 exists
        // to prevent.
        appLogger.Warning(summary);
        SetState(
            finished.status == goals::GoalStatus::Cancelled
                ? RuntimeState::Idle
                : RuntimeState::Blocked,
            summary);
    }
    return finished;
}

void ReviaSession::PublishGoalProgress(const goals::GoalProgress& progress) const
{
    RuntimeEvent event;
    event.kind = RuntimeEventKind::ComponentStatus;
    event.state = state.load();
    event.component = "Goal";
    event.phase = goals::ToString(progress.stepStatus);
    event.message = "[" + goals::ToString(progress.goalStatus) + "] step " +
        std::to_string(progress.ordinal + 1) + ": " + progress.message;
    eventBus.Publish(std::move(event));
}

std::string ReviaSession::FormatGoalSummary(const goals::Goal& goal)
{
    std::ostringstream stream;
    stream << "Goal '" << goal.title << "' " << goals::ToString(goal.status);
    if (goal.stopReason != goals::StopReason::None &&
        goal.stopReason != goals::StopReason::Completed)
    {
        stream << " (" << goals::ToString(goal.stopReason) << ")";
    }
    stream << ". Steps " << goal.currentStep << '/' << goal.steps.size()
        << ", actions " << goal.spend.actions << '/' << goal.budget.maxActions
        << ", retries " << goal.spend.retries << '/' << goal.budget.maxTotalRetries
        << ", elapsed " << goal.spend.elapsedMs << "ms.";
    return stream.str();
}

actions::CapabilitySettings ReviaSession::DeriveGoalScope() const
{
    // ExecuteScoped already takes the more restrictive of the global policy and this one,
    // so a goal cannot reach anything the profile could not. Narrowing here on top of that
    // keeps a long unattended-looking run from also being a broad one.
    return goals::NarrowScopeForGoal(actionRuntime.Settings());
}

bool ReviaSession::TryHandleGoalInput(const std::string& input, SessionResult& result)
{
    const std::string request = Trim(input.substr(6));
    if (request.empty())
    {
        result.succeeded = false;
        result.text = "Usage: /goal <what you want done>";
        result.reason = "No goal request was given.";
        SetState(RuntimeState::Blocked, result.reason);
        return true;
    }
    if (!actionRuntime.IsInitialized())
    {
        result.succeeded = false;
        result.text = "Action runtime is not initialized.";
        result.reason = result.text;
        SetState(RuntimeState::Blocked, result.reason);
        return true;
    }

    SetState(RuntimeState::Thinking, "Planning a goal.");
    const responseOutput proposal = router.PlanGoal(request);
    if (!proposal.bSuccess)
    {
        result.succeeded = false;
        result.text = proposal.response;
        result.reason = proposal.reason;
        SetState(RuntimeState::Error, result.reason);
        return true;
    }

    planning::ParsedGoal parsed = planning::GoalPlanner::ParseJson(proposal.response);
    if (!parsed.succeeded)
    {
        result.succeeded = false;
        result.text = "Goal plan rejected: " + parsed.error;
        result.reason = parsed.error;
        SetState(RuntimeState::Blocked, result.reason);
        return true;
    }

    parsed.goal.scope = DeriveGoalScope();
    for (goals::GoalStep& step : parsed.goal.steps)
    {
        step.action.requestedBy = "goal";
        step.check.requestedBy = "goal";
    }

    // Validate before asking, so a plan that could never complete is refused without
    // spending the user's attention on a confirmation prompt.
    std::string planError;
    if (!goals::GoalRunner::Validate(parsed.goal, planError))
    {
        result.succeeded = false;
        result.text = "Goal plan rejected: " + planError;
        result.reason = planError;
        SetState(RuntimeState::Blocked, result.reason);
        return true;
    }

    // Rehearse before asking. Approving a plan on the strength of how reasonable its text
    // reads is exactly what this stage is meant to replace, and a plan that cannot even
    // work on a copy should never reach the real folders or the user's attention.
    std::string rehearsalSummary;
    const goals::Goal rehearsed = RehearseGoal(parsed.goal, rehearsalSummary);
    if (rehearsed.status == goals::GoalStatus::Cancelled)
    {
        result.succeeded = false;
        result.text = "Goal cancelled during rehearsal; nothing real was touched.";
        result.reason = "The rehearsal was stopped.";
        SetState(RuntimeState::Idle, result.text);
        return true;
    }
    if (rehearsed.status == goals::GoalStatus::Failed ||
        rehearsed.status == goals::GoalStatus::Blocked ||
        rehearsed.status == goals::GoalStatus::Exhausted)
    {
        result.succeeded = false;
        result.text = rehearsalSummary + "\nNothing real was touched.";
        result.reason = goals::ToString(rehearsed.stopReason);
        appLogger.Warning("Goal refused after a failed rehearsal: " + rehearsalSummary);
        SetState(RuntimeState::Blocked, result.reason);
        return true;
    }

    // The plan is approved as a whole before any step runs. Individual steps still hit the
    // per-action confirmation path, so this adds a gate rather than replacing one.
    ConfirmationHandler handler;
    {
        std::lock_guard lock(confirmationMutex);
        handler = confirmationHandler;
    }
    if (handler)
    {
        actions::ActionRequest summary = parsed.goal.steps.front().action;
        summary.id = actions::NewActionId();
        summary.requestedBy = "goal";
        actions::PolicyDecision decision;
        decision.verdict = actions::PolicyVerdict::RequiresConfirmation;
        decision.reason = "Run the goal '" + parsed.goal.title + "' (" +
            std::to_string(parsed.goal.steps.size()) +
            (parsed.goal.steps.size() == 1 ? " step" : " steps") + ")?\n" +
            FormatGoalPlan(parsed.goal) + "\n" + rehearsalSummary;
        SetState(RuntimeState::WaitingForConfirmation, decision.reason);
        if (!handler(summary, decision))
        {
            parsed.goal.status = goals::GoalStatus::Cancelled;
            parsed.goal.stopReason = goals::StopReason::Cancelled;
            result.succeeded = false;
            result.text = "Goal cancelled before any step ran.";
            result.reason = "The goal plan was not approved.";
            SetState(RuntimeState::Idle, result.text);
            return true;
        }
    }

    const goals::Goal finished = RunGoalUnlocked(std::move(parsed.goal));
    result.succeeded = finished.status == goals::GoalStatus::Succeeded;
    result.text = FormatGoalSummary(finished);
    if (!result.succeeded)
    {
        result.reason = goals::ToString(finished.stopReason);
    }
    return true;
}

goals::Goal ReviaSession::RehearseGoal(const goals::Goal& goal, std::string& outSummary)
{
    goals::Goal rehearsed;
    const goals::SandboxRehearsal sandbox = goals::GoalSandbox::Prepare(goal);
    if (!sandbox.supported)
    {
        outSummary = "Not rehearsed: " + sandbox.reason + ".";
        rehearsed.status = goals::GoalStatus::Planned;
        return rehearsed;
    }
    // Disposal is structural rather than a call at the end of each path. A scratch tree
    // that survives its run is exactly the failure this class is named against, and an
    // exception between here and the end would otherwise leave one behind.
    struct ScratchGuard
    {
        std::filesystem::path root;
        logger& log;
        ~ScratchGuard()
        {
            std::string discardError;
            if (!goals::GoalSandbox::Discard(root, discardError))
            {
                log.Warning(
                    "The goal rehearsal directory could not be removed: " + discardError);
            }
        }
    } scratchGuard{sandbox.root, appLogger};

    if (!sandbox.prepared)
    {
        outSummary = "Rehearsal could not be set up: " + sandbox.reason + ".";
        rehearsed.status = goals::GoalStatus::Failed;
        return rehearsed;
    }

    SetState(RuntimeState::Acting, "Rehearsing the goal in a scratch copy.");
    // Its own runtime, policy, and audit log. Reusing the session's would block every step:
    // scoped execution takes the more restrictive of global and goal policy, and the
    // scratch directory is outside every configured approved root.
    actions::ActionRuntime rehearsalRuntime;
    std::string runtimeError;
    if (!rehearsalRuntime.Initialize(
        sandbox.capabilityConfig, sandbox.auditLog, runtimeError))
    {
        outSummary = "Rehearsal could not be set up: " + runtimeError;
        rehearsed.status = goals::GoalStatus::Failed;
        return rehearsed;
    }

    // Its own store too. A rehearsal is not history, and writing it to the real goal
    // database would put runs in /goals that never touched anything.
    const goals::GoalStore rehearsalStore((sandbox.root / "rehearsal.db").string());
    goals::GoalRunner rehearsalRunner(rehearsalRuntime, rehearsalStore);
    rehearsalRunner.SetProgressHandler([this](const goals::GoalProgress& progress)
    {
        goals::GoalProgress annotated = progress;
        annotated.message = "(rehearsal) " + annotated.message;
        PublishGoalProgress(annotated);
    });
    // Confined to a throwaway directory that only exists for this call, and the scope's
    // approved roots are that directory, so anything reaching outside is blocked by policy
    // rather than by asking. Prompting here would train the habit of approving a dialog
    // twice for one decision.
    rehearsalRunner.SetConfirmationHandler([](
        const actions::ActionRequest&,
        const actions::PolicyDecision&) { return true; });

    rehearsed = rehearsalRunner.Run(sandbox.goal, CurrentOperationToken());
    if (rehearsed.status == goals::GoalStatus::Succeeded)
    {
        outSummary = "Rehearsed successfully in a scratch copy: all " +
            std::to_string(rehearsed.steps.size()) +
            (rehearsed.steps.size() == 1 ? " step verified." : " steps verified.");
    }
    else
    {
        outSummary = "Rehearsal FAILED in a scratch copy - " + FormatGoalSummary(rehearsed);
        for (const goals::GoalStep& step : rehearsed.steps)
        {
            if (step.status == goals::StepStatus::Failed && !step.attempts.empty())
            {
                outSummary += "\n   step " + std::to_string(step.ordinal + 1) + ": " +
                    step.attempts.back().failure;
                break;
            }
        }
    }
    return rehearsed;
}

std::string ReviaSession::FormatGoalPlan(const goals::Goal& goal)
{
    std::ostringstream stream;
    for (const goals::GoalStep& step : goal.steps)
    {
        // Marked per step because a plan is approved as a whole. Asked something vague the
        // planner will propose deletion inside an otherwise ordinary-looking plan, and
        // recycling is classified ReversibleWrite, so nothing else in the pipeline makes
        // it stand out. This prompt is where the user sees it or does not see it at all.
        stream << (step.ordinal + 1) << ". " << step.description;
        if (step.action.type == actions::ActionType::MoveToRecycleBin)
        {
            stream << "  [DELETES FILES]";
        }
        else if (actions::RiskForAction(step.action.type) == actions::RiskLevel::Destructive)
        {
            stream << "  [DESTRUCTIVE]";
        }
        stream << "\n   " << actions::ToString(step.action.type);
        if (!step.action.source.empty())
        {
            stream << ' ' << actions::PathToUtf8(step.action.source);
        }
        if (!step.action.destination.empty())
        {
            stream << " -> " << actions::PathToUtf8(step.action.destination);
        }
        if (!step.action.application.empty())
        {
            stream << ' ' << step.action.application;
        }
        stream << "\n   verify with " << actions::ToString(step.check.type)
            << " expecting \"" << step.expected << "\"\n";
    }
    return stream.str();
}

std::string ReviaSession::FormatGoalList(const std::vector<goals::Goal>& goalList)
{
    if (goalList.empty())
    {
        return "No goals have been recorded yet.";
    }
    std::ostringstream stream;
    for (const goals::Goal& goal : goalList)
    {
        stream << goal.id << "  " << goals::ToString(goal.status) << "  " << goal.title;
        if (!goals::IsTerminal(goal.status))
        {
            stream << "  (resumable: /goals resume " << goal.id << ")";
        }
        stream << '\n';
    }
    return stream.str();
}

bool ReviaSession::TryHandleActionInput(const std::string& input, SessionResult& result)
{
    if (input == "/capabilities")
    {
        result.text = actionRuntime.StatusJson();
        SetState(RuntimeState::Idle);
        return true;
    }

    if (input.rfind("/goals resume ", 0) == 0)
    {
        const std::string goalId = Trim(input.substr(14));
        if (goalId.empty())
        {
            result.succeeded = false;
            result.text = "Usage: /goals resume <goal-id>";
            result.reason = "No goal id was given.";
            SetState(RuntimeState::Blocked, result.reason);
            return true;
        }
        const goals::Goal finished = ResumeGoalUnlocked(goalId);
        result.succeeded = finished.status == goals::GoalStatus::Succeeded;
        result.text = FormatGoalSummary(finished);
        if (!result.succeeded)
        {
            result.reason = goals::ToString(finished.stopReason);
        }
        return true;
    }

    if (input == "/goals")
    {
        result.text = FormatGoalList(goalStore.LoadRecent());
        SetState(RuntimeState::Idle);
        return true;
    }

    if (input.rfind("/goal ", 0) == 0)
    {
        return TryHandleGoalInput(input, result);
    }

    if (input == "/bargein" || input.rfind("/bargein ", 0) == 0)
    {
        const std::string argument = input.size() > 9 ? Trim(input.substr(9)) : std::string();
        if (argument == "on" || argument == "off")
        {
            SetBargeInEnabled(argument == "on");
        }
        else if (!argument.empty())
        {
            result.succeeded = false;
            result.text = "Usage: /bargein [on|off]";
            result.reason = "Unrecognized barge-in argument.";
            SetState(RuntimeState::Blocked, result.reason);
            return true;
        }
        result.text = speechService.IsBargeInEnabled()
            ? "Barge-in is ON. Speaking over Revia stops her mid-sentence."
            : "Barge-in is OFF. Revia finishes what she is saying.";
        SetState(RuntimeState::Idle);
        return true;
    }

    if (input == "/review" || input.rfind("/review ", 0) == 0)
    {
        const std::string argument = input.size() > 7 ? Trim(input.substr(7)) : std::string();
        if (argument.rfind("accept", 0) == 0)
        {
            const std::string lessonId = Trim(argument.substr(6));
            if (lessonId.empty())
            {
                result.succeeded = false;
                result.text = "Usage: /review accept <lesson-id>";
                result.reason = "No lesson id was given.";
                SetState(RuntimeState::Blocked, result.reason);
                return true;
            }
            std::string summary;
            result.succeeded = ApproveLesson(lessonId, summary);
            result.text = summary;
            if (!result.succeeded)
            {
                result.reason = summary;
            }
            SetState(RuntimeState::Idle);
            return true;
        }
        if (!argument.empty())
        {
            result.succeeded = false;
            result.text = "Usage: /review [accept <lesson-id>]";
            result.reason = "Unrecognized review argument.";
            SetState(RuntimeState::Blocked, result.reason);
            return true;
        }

        const std::vector<learning::Lesson> lessons = DrawLessons();
        std::ostringstream stream;
        if (lessons.empty())
        {
            stream << "Nothing to review yet. Lessons need at least "
                << learning::LearningReview::MinimumSamples
                << " finished goals or judged proposals before a pattern means anything.";
        }
        else
        {
            stream << "Lessons drawn from what has actually happened. Nothing is "
                      "remembered until you approve it, and approving one stores a note "
                      "-- it never changes a capability or a budget.";
            for (const learning::Lesson& lesson : lessons)
            {
                stream << "\n\n  " << lesson.id << "  ["
                    << learning::ToString(lesson.kind) << "]\n  "
                    << lesson.statement << "\n  because " << lesson.evidence
                    << "\n  /review accept " << lesson.id;
            }
        }
        result.text = stream.str();
        SetState(RuntimeState::Idle);
        return true;
    }

    if (input == "/initiative" || input.rfind("/initiative ", 0) == 0)
    {
        const std::string argument = input.size() > 11 ? Trim(input.substr(11)) : std::string();
        if (argument.rfind("accept", 0) == 0 || argument.rfind("dismiss", 0) == 0)
        {
            const bool accepting = argument.rfind("accept", 0) == 0;
            std::string proposalId = Trim(argument.substr(accepting ? 6 : 7));
            if (proposalId.empty())
            {
                const auto pending = initiativeController.Pending();
                if (pending.empty())
                {
                    result.text = "There is nothing waiting for an answer.";
                    SetState(RuntimeState::Idle);
                    return true;
                }
                proposalId = pending.back().id;
            }
            if (accepting)
            {
                result = AcceptProposal(proposalId);
            }
            else
            {
                DismissProposal(proposalId);
                result.text = "Dismissed. I will wait longer before offering again.";
            }
            SetState(RuntimeState::Idle);
            return true;
        }
        if (!argument.empty())
        {
            result.succeeded = false;
            result.text = "Usage: /initiative [accept|dismiss] [proposal-id]";
            result.reason = "Unrecognized initiative argument.";
            SetState(RuntimeState::Blocked, result.reason);
            return true;
        }
        std::ostringstream stream;
        stream << InitiativeStatus();
        const auto pending = initiativeController.Pending();
        if (!pending.empty())
        {
            stream << "\nWaiting on you:";
            for (const initiative::Proposal& proposal : pending)
            {
                stream << "\n  " << proposal.id << "  " << proposal.message
                    << "\n      because " << proposal.evidence;
            }
        }
        result.text = stream.str();
        SetState(RuntimeState::Idle);
        return true;
    }

    if (input == "/perception" || input.rfind("/perception ", 0) == 0)
    {
        const std::string argument = input.size() > 11 ? Trim(input.substr(11)) : std::string();
        if (argument == "forget")
        {
            ForgetActivity();
            result.text = "Observation history cleared. Nothing about earlier windows "
                "remains in memory.";
            SetState(RuntimeState::Idle);
            return true;
        }
        if (argument.rfind("history", 0) == 0)
        {
            std::chrono::minutes window{60};
            const std::string tail = Trim(argument.substr(7));
            if (!tail.empty())
            {
                try
                {
                    const int requested = std::stoi(tail);
                    // Clamped rather than rejected: asking for a longer window than is
                    // retained is a reasonable thing to do, and silently answering from
                    // less data is worse than answering the question that can be answered.
                    window = std::chrono::minutes{std::clamp(requested, 1, 480)};
                }
                catch (const std::exception&)
                {
                    result.succeeded = false;
                    result.text = "Usage: /perception history [minutes]";
                    result.reason = "The history window must be a number of minutes.";
                    SetState(RuntimeState::Blocked, result.reason);
                    return true;
                }
            }
            result.text = RecentActivity(window);
            SetState(RuntimeState::Idle);
            return true;
        }
        if (argument == "pause" || argument == "resume")
        {
            if (!settings.perception.bEnabled)
            {
                result.succeeded = false;
                result.text = "Ambient perception is off, so there is nothing to " +
                    argument + ".";
                result.reason = "Perception is disabled in settings.";
                SetState(RuntimeState::Blocked, result.reason);
                return true;
            }
            windowEventMonitor.SetPaused(argument == "pause");
        }
        else if (!argument.empty())
        {
            result.succeeded = false;
            result.text = "Usage: /perception [pause|resume|history [minutes]|forget]";
            result.reason = "Unrecognized perception argument.";
            SetState(RuntimeState::Blocked, result.reason);
            return true;
        }
        result.text = PerceptionStatus();
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

std::stop_token ReviaSession::CurrentOperationToken() const
{
    std::lock_guard lock(cancellationMutex);
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
