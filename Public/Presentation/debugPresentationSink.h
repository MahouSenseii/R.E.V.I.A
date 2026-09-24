#pragma once

#include "Presentation/avatarState.h"
#include "Presentation/presentationBus.h"

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace revia::presentation
{

// A renderer made of text.
//
// It exists to prove the boundary is real before any avatar does. If a debug window can
// show what Revia is doing using only presentation events, then a Live2D model can too,
// and the core never learns the difference. If it cannot, the events are missing
// something -- and finding that out now costs a line of text rather than a rig.
class DebugPresentationSink : public IPresentationSink
{
public:
    using Writer = std::function<void(const std::string&)>;

    explicit DebugPresentationSink(Writer writer = {});

    void OnPresentation(const PresentationEvent& event) override;
    [[nodiscard]] std::string Name() const override { return "DebugPresentation"; }

    // The lines it would have drawn, newest last. Bounded.
    [[nodiscard]] std::vector<std::string> Lines() const;

    // One line describing visible state, the way an overlay would.
    [[nodiscard]] static std::string Describe(const AvatarState& state);

private:
    mutable std::mutex mutex;
    Writer writer;
    std::deque<std::string> lines;
};

} // namespace revia::presentation
