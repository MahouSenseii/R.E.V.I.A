#pragma once

#include "Presentation/presentationEvents.h"
#include "Runtime/runtimeEvents.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::presentation
{

// Anything that can show Revia. A debug console now; a Live2D model, a VRM rig, or an
// OBS overlay later, without the core learning about any of them.
class IPresentationSink
{
public:
    virtual ~IPresentationSink() = default;

    // Called on the publishing thread. A sink must not block and must not publish back
    // into the bus.
    virtual void OnPresentation(const PresentationEvent& event) = 0;

    // For diagnostics listings.
    [[nodiscard]] virtual std::string Name() const = 0;
};

class IPresentationBus
{
public:
    virtual ~IPresentationBus() = default;
    virtual void Publish(PresentationEvent event) = 0;
};

class PresentationBus : public IPresentationBus
{
public:
    using SubscriptionId = std::uint64_t;

    SubscriptionId Add(std::shared_ptr<IPresentationSink> sink);
    void Remove(SubscriptionId id);

    // Stamps the sequence and timestamp, then fans out. Subject sanitisation happens
    // here so no publisher can opt out of it.
    void Publish(PresentationEvent event) override;

    [[nodiscard]] std::vector<std::string> SinkNames() const;
    [[nodiscard]] std::uint64_t Published() const;

private:
    mutable std::mutex mutex;
    std::vector<std::pair<SubscriptionId, std::shared_ptr<IPresentationSink>>> sinks;
    SubscriptionId nextId = 1;
    std::uint64_t sequence = 0;
};

// Narrows runtime events to visible presentation meaning; unrelated events return nothing.
// Private SelfInquiry reasoning and evidence detail must not cross this boundary.
[[nodiscard]] std::optional<PresentationEvent> TranslateRuntimeEvent(const runtime::RuntimeEvent& event);

} // namespace revia::presentation
