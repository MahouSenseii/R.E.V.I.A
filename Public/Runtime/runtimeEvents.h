#pragma once

#include "Runtime/affectTypes.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace revia::runtime
{

enum class RuntimeState
{
    Offline,
    Starting,
    Idle,
    Thinking,
    Responding,
    Remembering,
    Acting,
    WaitingForConfirmation,
    Blocked,
    Error,
    Stopping
};

enum class RuntimeEventKind
{
    StateChanged,
    Activity,
    AssistantMessage,
    AffectChanged,
    ComponentStatus,
    Memory,
    Timing,
    // Read-only hardware inventory and resolved workload assignments for the Resources
    // tab. Kept distinct from ComponentStatus so hardware rows do not become pipelines.
    ResourceStatus,
    // One complete sentence of a reply, published as soon as it exists so the shell can
    // show it in step with the audio instead of after the whole reply is generated.
    ReplyFragment,
    // Revia offering something unprompted. Distinct from AssistantMessage because it is
    // answerable: the shell must be able to accept or dismiss it in one action.
    Proposal,
    // A sanitized SVG drawing, ready to render. `detail` carries the markup and
    // `resource` the file it was saved to. Distinct from AssistantMessage because a
    // picture belongs on a canvas, not in the middle of a chat transcript.
    Diagram,
    Error
};

struct RuntimeEvent
{
    RuntimeEvent() = default;
    RuntimeEvent(
        RuntimeEventKind inputKind,
        RuntimeState inputState,
        std::string inputMessage,
        std::uint64_t inputTurnId = 0)
        : kind(inputKind), state(inputState), message(std::move(inputMessage)), turnId(inputTurnId)
    {
    }

    RuntimeEventKind kind = RuntimeEventKind::Activity;
    RuntimeState state = RuntimeState::Offline;
    std::string message;
    std::uint64_t turnId = 0;
    AffectState affect = AffectState::Neutral;
    float affectIntensity = 0.0F;
    std::string component;
    std::string phase;
    // Exact compute assignment such as CUDA0, cuda:1, or CPU. Component events keep the
    // last non-empty value so transient state updates do not erase startup placement.
    std::string resource;
    // Supporting text. For a proposal this is the evidence behind it, so the user can
    // judge the reasoning rather than only the conclusion.
    std::string detail;
    double elapsedMilliseconds = -1.0;
    int queueDepth = 0;
    std::uint64_t totalMemoryMiB = 0;
    std::uint64_t availableMemoryMiB = 0;
    std::uint64_t allocatedMemoryMiB = 0;
    // Live usage against the budget the resource plan set aside, for ResourceStatus
    // events in the "Usage" phase. Doubles rather than integers because CPU load is a
    // fraction of a thread, and unit tells the shell whether to render MiB or threads.
    double usedAmount = 0.0;
    double budgetAmount = 0.0;
    double capacityAmount = 0.0;
    std::string usageUnit;
    // False when the platform could not report the reading. The shell must show that
    // rather than a zero, which would read as "nothing is using it".
    bool usageMeasured = false;
    std::chrono::system_clock::time_point occurredAt = std::chrono::system_clock::now();
};

std::string ToString(RuntimeState state);

class RuntimeEventBus
{
public:
    using Handler = std::function<void(const RuntimeEvent&)>;
    using SubscriptionId = std::uint64_t;

    SubscriptionId Subscribe(Handler handler);
    void Unsubscribe(SubscriptionId id);
    void Publish(RuntimeEvent event) const;

private:
    mutable std::mutex mutex;
    mutable std::unordered_map<SubscriptionId, Handler> handlers;
    SubscriptionId nextId = 1;
};

} // namespace revia::runtime
