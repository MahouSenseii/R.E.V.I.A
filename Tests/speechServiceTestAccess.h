#pragma once

#include "testSupport.h"
#include "Speech/speechService.h"
#include <functional>
#include <chrono>
#include <thread>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>

namespace revia::speech
{

// Holds the pipeline between generation and playback without starting audio or
// inference workers. Speak and SynthesizeOne still produce the queued item and
// its lifetime; the test does not manufacture a persistent prepared utterance.
struct SpeechServiceTestAccess
{
    static bool CanLockState(SpeechService& service)
    {
        std::unique_lock lock(service.mutex, std::try_to_lock);
        return lock.owns_lock();
    }
    static QwenTtsPool& VoicePool(SpeechService& service)
    {
        return service.qwenPool;
    }
    static std::jthread StartGenerationWithoutPlayback(SpeechService& service)
    {
        return std::jthread([&service](const std::stop_token token) { service.Generate(token); });
    }
    static void ConsumePreparedWithoutPlayback(SpeechService& service)
    {
        std::lock_guard lock(service.mutex);
        while (const auto sequence = service.playbackOrder.PopFrontReady())
        {
            const auto found = service.prepared.find(*sequence);
            if (found == service.prepared.end())
                continue;
            service.bufferedAudioBytes -= found->second.bufferedBytes;
            SpeechService::ReleaseAudio(found->second.audioPath, found->second.lifetime);
            service.prepared.erase(found);
        }
        service.condition.notify_all();
    }
    static std::function<void(const std::function<bool(const std::filesystem::path&)>&)> TakeCuePlayback(SpeechService& service)
    {
        SpeechService::PreparedUtterance item;
        {
            std::lock_guard lock(service.mutex);
            const auto sequence = service.playbackOrder.PopFrontReady();
            tests::Check(sequence.has_value(), "No real prepared cue to play.");
            item = std::move(service.prepared.at(*sequence));
            service.prepared.erase(*sequence);
            tests::Check(item.utterance.systemCue.has_value(), "The prepared item was not a system cue.");
            service.playingAudio = true;
        }
        return [&service, item = std::move(item)](const auto& player)
        {
            service.PlayBankClip(item, player);
            std::lock_guard lock(service.mutex);
            service.playingAudio = false;
            service.condition.notify_all();
        };
    }
    static void AssignAdapterTestVoice(SpeechService& service, VoicePreset preset = {})
    {
        service.configuration.bEnabled = true;
        service.configuration.backend = "Qwen";
        service.activePreset = std::move(preset);
    }
    static std::vector<runtime::AffectSnapshot> PendingAffects(SpeechService& service)
    {
        std::lock_guard lock(service.mutex);
        std::vector<runtime::AffectSnapshot> result;
        for (const auto& item : service.queue) result.push_back(item.affect);
        return result;
    }

    static void ConfigureWithoutWorkers(SpeechService& service, const std::filesystem::path& root)
    {
        service.Shutdown();
        service.configuration.backend = "WindowsSapi";
        service.configuration.voiceDataPath = root.string();
        service.presetStore.SetRoot(root);
        service.voiceShutdown = false;
        service.enabled.store(true);
    }

    static void ConfigureSynthesisWithoutWorkers(
        SpeechService& service, const speechSettings& settings, VoicePreset preset, SpeechService::EventHandler handler)
    {
        ConfigureWithoutWorkers(service, settings.voiceDataPath);
        service.configuration = settings;
        service.eventHandler = std::move(handler);
        service.qwenPool.Configure(settings);
        service.UseVoice(std::move(preset));
    }

    // How many queued phrases would join a batch led by a follow-on phrase, with the
    // given real-time factor measured on one card (none when factor <= 0).
    static std::size_t BatchCompanionsWith(SpeechService& service, const double factor)
    {
        std::lock_guard lock(service.mutex);
        service.configuration.bQwenBatchReplyPhrases = true;
        service.configuration.bQwenDirectPcm = true;
        service.configuration.qwenMaxBatchPhrases = 6;
        service.configuration.qwenMaxBatchCharacters = 480;
        const auto phrase = [](std::string text)
        {
            SpeechService::Utterance utterance;
            utterance.text = std::move(text);
            utterance.generation = 7;
            utterance.latencyCritical = false;
            utterance.preset = VoicePreset{};
            return utterance;
        };
        service.queue.clear();
        service.queue.push_back(phrase("A third complete sentence."));
        service.queue.push_back(phrase("And a fourth one after it."));
        service.measuredRealTimeFactor.clear();
        if (factor > 0.0) service.measuredRealTimeFactor["test card"] = factor;
        const auto companions = service.CollectBatchCompanions(phrase("A second sentence."));
        service.queue.clear();
        service.measuredRealTimeFactor.clear();
        return companions.size();
    }

    // Discards a two-phrase batch the way a finished-but-interrupted batch is discarded,
    // on its own thread. Returns false if it did not come back within the timeout, which
    // is what the self-deadlock looked like; `phase` receives the event it published.
    static bool DiscardBatchReturns(SpeechService& service, std::string& phase)
    {
        auto seen = std::make_shared<std::string>();
        {
            std::lock_guard lock(service.mutex);
            service.eventHandler = [seen](const SpeechEvent& event) { *seen = event.phase; };
        }
        std::vector<SpeechService::Utterance> group(2);
        group[0].utteranceId = 1;
        group[1].utteranceId = 1;
        auto finished = std::make_shared<std::atomic<bool>>(false);
        std::thread worker([&service, group, finished]
        {
            service.DiscardBatch(group, 10.0, 0);
            finished->store(true);
        });
        for (int waited = 0; waited < 200 && !finished->load(); ++waited)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!finished->load())
        {
            // Leave it running: joining a deadlocked thread would hang the whole suite.
            worker.detach();
            return false;
        }
        worker.join();
        phase = *seen;
        return true;
    }

    static std::function<void()> TakeNext(SpeechService& service)
    {
        std::lock_guard lock(service.mutex);
        tests::Check(!service.queue.empty(), "Speak did not enqueue the expected segment.");
        auto utterance = std::move(service.queue.front());
        service.queue.pop_front();
        ++service.generatingCount;
        return [&service, utterance = std::move(utterance)]() mutable
        {
            service.SynthesizeOne(std::move(utterance), 0);
        };
    }

    static std::function<void(VoiceOperationResult)> StartNextSynthesis(SpeechService& service)
    {
        SpeechService::Utterance utterance;
        {
            std::lock_guard lock(service.mutex);
            tests::Check(!service.queue.empty(), "No queued phrase for controlled completion.");
            utterance = std::move(service.queue.front());
            service.queue.pop_front();
            ++service.generatingCount;
        }
        tests::Check(service.BeginSynthesis(utterance), "The controlled attempt did not start.");
        return [&service, utterance = std::move(utterance)](VoiceOperationResult result)
        {
            SpeechService::PreparedUtterance item;
            item.utterance = utterance;
            item.qwenAttempted = true;
            item.result = std::move(result);
            service.PublishGenerated(utterance, std::move(item), 0);
        };
    }

    static void SynthesizeQueuedBatch(SpeechService& service)
    {
        std::vector<SpeechService::Utterance> group;
        {
            std::lock_guard lock(service.mutex);
            while (!service.queue.empty())
            {
                group.push_back(std::move(service.queue.front()));
                service.queue.pop_front();
                ++service.generatingCount;
            }
        }
        tests::Check(group.size() >= 2, "The batch fixture needs at least two phrases.");
        if (!service.SynthesizeBatch(group, 0))
            for (auto& utterance : group)
                service.SynthesizeOne(std::move(utterance), 0);
    }

    static VoiceOperationResult SynthesisException()
    {
        return SpeechService::TrySynthesis([]() -> VoiceOperationResult { throw std::runtime_error("PRIVATE_SYNTHESIS_SENTINEL"); });
    }

    static std::size_t HealthScopeCount(SpeechService& service)
    {
        std::lock_guard lock(service.mutex);
        return service.synthesisStates.size();
    }

    static std::size_t HealthNotificationCount(SpeechService& service)
    {
        std::lock_guard lock(service.mutex);
        return service.synthesisNotifications.size() + (service.pendingSynthesisStatus ? 1 : 0);
    }

    static void NotifyReady(SpeechService& service)
    {
        service.Notify({"Ready", "A preparation fixture completed."});
    }

    static void SetPlayingTurn(SpeechService& service, const std::uint64_t turn)
    {
        service.activeUtteranceId.store(turn);
    }

    static void RestartHealthWithoutWorkers(SpeechService& service)
    {
        service.Shutdown();
        {
            std::lock_guard lock(service.mutex);
            service.voiceShutdown = false;
            service.enabled.store(true);
            service.ResetVerifiedSynthesisLocked();
            service.QueueSynthesisStatusLocked();
        }
        service.DrainSynthesisNotifications();
    }

    static void SetNextSequence(SpeechService& service, const std::uint64_t sequence)
    {
        service.nextSequence.store(sequence);
    }

    static std::function<void()> TakeNextAndCollectBatch(SpeechService& service, std::size_t& companionCount)
    {
        std::vector<SpeechService::Utterance> group;
        {
            std::lock_guard lock(service.mutex);
            tests::Check(!service.queue.empty(), "No queued batch leader.");
            group.push_back(std::move(service.queue.front()));
            service.queue.pop_front();
            ++service.generatingCount;
            auto companions = service.CollectBatchCompanions(group.front());
            companionCount = companions.size();
            service.generatingCount += companionCount;
            for (auto& companion : companions)
                group.push_back(std::move(companion));
        }
        return [&service, group = std::move(group)]() mutable
        {
            if (group.size() > 1 && service.SynthesizeBatch(group, 0))
                return;
            for (auto& utterance : group)
                service.SynthesizeOne(std::move(utterance), 0);
        };
    }

    static bool HasPreparedClip(SpeechService& service, const std::filesystem::path& path)
    {
        std::lock_guard lock(service.mutex);
        for (const auto& [sequence, item] : service.prepared)
        {
            (void)sequence;
            if (item.audioPath == path &&
                item.lifetime == SpeechService::AudioLifetime::Persistent &&
                item.utterance.vocalization.has_value() && item.utterance.text.empty())
            {
                return true;
            }
        }
        return false;
    }

    static void AddTemporaryAudio(SpeechService& service, const std::filesystem::path& path)
    {
        std::lock_guard lock(service.mutex);
        SpeechService::PreparedUtterance item;
        item.utterance.sequence = service.nextSequence.fetch_add(1);
        item.utterance.generation = service.generation.load();
        item.audioPath = path;
        item.bufferedBytes = 64;
        const auto sequence = item.utterance.sequence;
        service.playbackOrder.Reserve(sequence);
        service.playbackOrder.MarkReady(sequence);
        service.bufferedAudioBytes += item.bufferedBytes;
        service.prepared.emplace(sequence, std::move(item));
    }

    static bool QueuesCleared(SpeechService& service)
    {
        std::lock_guard lock(service.mutex);
        return service.queue.empty() && service.prepared.empty() &&
            service.playbackOrder.Empty() && service.bufferedAudioBytes == 0 &&
            service.generatingCount == 0;
    }
};

} // namespace revia::speech
