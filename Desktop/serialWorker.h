#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace revia::desktop
{
// One background thread that runs posted work in order. The Settings dialog writes through it, so
// two writes never race each other and a write is never left running against a session that is
// being retired: Stop() lets the work already posted finish, then joins. Work posted after Stop()
// is dropped. Start() makes a stopped worker accept work again (a companion switch stops it before
// the old session goes and starts it for the new one).
class SerialWorker
{
  public:
    SerialWorker()
    {
        Start();
    }
    ~SerialWorker()
    {
        Stop();
    }
    SerialWorker(const SerialWorker&) = delete;
    SerialWorker& operator=(const SerialWorker&) = delete;

    void Start()
    {
        std::lock_guard lock(mutex);
        if (thread.joinable())
            return;
        stopping = false;
        thread = std::jthread([this]() { Run(); });
    }

    void Post(std::function<void()> work)
    {
        {
            std::lock_guard lock(mutex);
            if (stopping || !thread.joinable())
                return;
            queue.push_back(std::move(work));
        }
        wake.notify_one();
    }

    // Finishes what was posted, then joins. Never call it from the worker itself.
    void Stop()
    {
        std::jthread finishing;
        {
            std::lock_guard lock(mutex);
            stopping = true;
            finishing = std::move(thread);
        }
        wake.notify_one();
        if (finishing.joinable())
            finishing.join();
    }

  private:
    void Run()
    {
        for (;;)
        {
            std::function<void()> work;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this]() { return stopping || !queue.empty(); });
                if (queue.empty())
                    return;
                work = std::move(queue.front());
                queue.pop_front();
            }
            // A throwing write must not end the thread, or every later write would wait for ever.
            try
            {
                work();
            }
            catch (...)
            {
            }
        }
    }

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> queue;
    std::jthread thread;
    bool stopping = false;
};
}
