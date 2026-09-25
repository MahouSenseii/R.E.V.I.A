#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace revia::core
{

// Asks the user something on the UI thread for a worker, without ever waiting on a UI
// thread that has stopped listening.
//
// A worker that blocks until the UI thread shows its dialog deadlocks as soon as the UI
// thread waits for that worker instead, and shutdown does exactly that: it joins the
// workers. Abandon() breaks the cycle. Every question still waiting gets the refusal,
// and nothing is asked after it.
class QuestionRelay
{
public:
    // Queues work on the UI thread. It may never run: Qt drops it once the window is gone.
    using Post = std::function<void(std::function<void()>)>;

    // Returns ask()'s answer, or `refused` if shutdown began first or while waiting.
    // On the UI thread itself it asks directly; posting to itself and waiting would never
    // return. `ask` runs after the caller may have given up, so it must own what it uses.
    template <typename Answer>
    Answer Ask(const Post& post, const bool onUiThread, std::function<Answer()> ask,
        const Answer refused)
    {
        if (onUiThread) return IsAbandoned() ? refused : ask();

        struct Pending
        {
            bool done = false;
            Answer answer;
        };
        auto pending = std::make_shared<Pending>(Pending{false, refused});
        {
            std::lock_guard lock(mutex);
            if (abandoned) return refused;
        }
        post([this, pending, ask = std::move(ask)]()
        {
            // Nobody is waiting any more, so there is nothing to ask.
            if (IsAbandoned()) return;
            Answer answer = ask();
            {
                std::lock_guard lock(mutex);
                pending->answer = std::move(answer);
                pending->done = true;
            }
            changed.notify_all();
        });
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return pending->done || abandoned; });
        return pending->done ? pending->answer : refused;
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

    [[nodiscard]] bool IsAbandoned() const
    {
        std::lock_guard lock(mutex);
        return abandoned;
    }

private:
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool abandoned = false;
};

} // namespace revia::core
