#pragma once

#include "testSupport.h"
#include "Speech/speechService.h"
#include <functional>
#include <mutex>
#include <vector>

namespace revia::speech
{

// Holds the pipeline between generation and playback without starting audio or
// inference workers. Speak and SynthesizeOne still produce the queued item and
// its lifetime; the test does not manufacture a persistent prepared utterance.
struct SpeechServiceTestAccess
{
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
