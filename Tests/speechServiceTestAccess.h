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
    static QwenTtsPool& VoicePool(SpeechService& service) { return service.qwenPool; }
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
        service.enabled.store(true);
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
