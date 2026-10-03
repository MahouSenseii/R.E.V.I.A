#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace revia::core
{

// Relays worker questions to the UI without shutdown deadlocks.
// Abandon refuses pending questions and prevents new ones before workers are joined.
class QuestionRelay
{
  public:
    // Queues work on the UI thread. It may never run: Qt drops it once the window is gone.
    using Post = std::function<void(std::function<void()>)>;

    // Returns ask()'s answer, or `refused` if shutdown began first or while waiting.
    // On the UI thread itself it asks directly; posting to itself and waiting would never
    // return. `ask` runs after the caller may have given up, so it must own what it uses.
    template <typename Answer> Answer Ask(const Post& post, const bool onUiThread, std::function<Answer()> ask, const Answer refused)
    {
        std::uint64_t requestGeneration;
        {
            std::lock_guard lock(mutex);
            if (abandoned)
                return refused;
            requestGeneration = generation;
        }
        if (onUiThread)
        {
            Answer answer = ask();
            return IsCurrent(requestGeneration) ? answer : refused;
        }

        struct Pending
        {
            bool done = false;
            Answer answer;
        };
        auto pending = std::make_shared<Pending>(Pending{false, refused});
        post(
            [this, pending, requestGeneration, ask = std::move(ask)]()
            {
                if (!IsCurrent(requestGeneration))
                    return;
                Answer answer = ask();
                {
                    std::lock_guard lock(mutex);
                    if (abandoned || generation != requestGeneration)
                        return;
                    pending->answer = std::move(answer);
                    pending->done = true;
                }
                changed.notify_all();
            });
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return pending->done || abandoned || generation != requestGeneration; });
        return pending->done && !abandoned && generation == requestGeneration ? pending->answer : refused;
    }

    // Shutdown: waiting questions return their refusal now, and later ones at once.
    void Abandon()
    {
        {
            std::lock_guard lock(mutex);
            abandoned = true;
        }
        changed.notify_all();
    }

    // Begin fresh questions after outgoing workers have joined. Old queued work stays refused.
    void Reopen()
    {
        {
            std::lock_guard lock(mutex);
            ++generation;
            abandoned = false;
        }
        changed.notify_all();
    }

    [[nodiscard]] bool IsAbandoned() const
    {
        std::lock_guard lock(mutex);
        return abandoned;
    }

  private:
    [[nodiscard]] bool IsCurrent(const std::uint64_t requestGeneration) const
    {
        std::lock_guard lock(mutex);
        return !abandoned && generation == requestGeneration;
    }

    mutable std::mutex mutex;
    std::condition_variable changed;
    bool abandoned = false;
    std::uint64_t generation = 0;
};

} // namespace revia::core
