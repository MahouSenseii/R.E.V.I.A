#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

namespace revia::llm
{

enum class InferencePriority
{
    Interactive,
    Background
};

struct InferenceSchedulerSnapshot
{
    int capacity = 1;
    int active = 0;
    int activeBackground = 0;
    int waitingInteractive = 0;
    int waitingBackground = 0;
};

// Capacity gate for the shared chat/vision llama.cpp server.
//
// Independent pipelines may submit concurrently, but they must honor the number of
// server slots actually reported by llama.cpp. Waiting interactive work always enters
// before waiting background memory work; the dedicated embedding server does not use
// this gate at all.
class InferenceScheduler
{
public:
    class Lease
    {
    public:
        Lease() = default;
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        [[nodiscard]] explicit operator bool() const;
        [[nodiscard]] std::stop_token PreemptionToken() const;

    private:
        friend class InferenceScheduler;
        Lease(
            InferenceScheduler* owner,
            InferencePriority priority,
            std::shared_ptr<std::stop_source> preemptionSource = {});
        void Reset();

        InferenceScheduler* owner = nullptr;
        InferencePriority priority = InferencePriority::Interactive;
        std::shared_ptr<std::stop_source> preemptionSource;
    };

    void SetCapacity(int slots);
    [[nodiscard]] Lease Acquire(
        InferencePriority priority,
        std::stop_token stopToken = {});
    [[nodiscard]] InferenceSchedulerSnapshot Snapshot() const;

private:
    void Release(InferencePriority priority);

    mutable std::mutex mutex;
    std::condition_variable_any available;
    int capacity = 1;
    int active = 0;
    int activeBackground = 0;
    int waitingInteractive = 0;
    int waitingBackground = 0;
    std::vector<std::weak_ptr<std::stop_source>> backgroundCancellations;
};

} // namespace revia::llm
