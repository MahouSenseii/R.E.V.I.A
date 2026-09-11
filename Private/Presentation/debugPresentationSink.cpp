#include "Presentation/debugPresentationSink.h"

#include "Runtime/affectTypes.h"

#include <sstream>
#include <utility>

namespace revia::presentation
{

namespace
{

constexpr std::size_t LineLimit = 128;

} // namespace

DebugPresentationSink::DebugPresentationSink(Writer output)
    : writer(std::move(output))
{
}

void DebugPresentationSink::OnPresentation(const PresentationEvent& event)
{
    std::ostringstream line;
    line << '#' << event.sequence << ' ' << ToString(event.kind);
    if (!event.subject.empty()) line << " \"" << event.subject << '"';
    if (event.affect != runtime::AffectState::Neutral)
    {
        line << " [" << runtime::ToString(event.affect) << ']';
    }

    std::string rendered = line.str();
    Writer copy;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        lines.push_back(rendered);
        while (lines.size() > LineLimit) lines.pop_front();
        copy = writer;
    }
    if (copy) copy(rendered);
}

std::vector<std::string> DebugPresentationSink::Lines() const
{
    const std::lock_guard<std::mutex> lock(mutex);
    return {lines.begin(), lines.end()};
}

std::string DebugPresentationSink::Describe(const AvatarState& state)
{
    std::ostringstream description;
    description << runtime::ToString(state.expression) << ' '
                << static_cast<int>(state.expressionIntensity * 100.0F) << "%"
                << ", gaze " << ToString(state.gaze)
                << ", idle " << ToString(state.idle);
    if (state.speaking) description << ", speaking";
    if (state.listening) description << ", listening";
    if (state.thinking) description << ", thinking";
    if (state.singing) description << ", singing";
    if (state.researching) description << ", researching";
    if (state.computerTask) description << ", at the computer";
    if (state.lipSync) description << ", mouth active";
    if (!state.activity.empty()) description << " (" << state.activity << ')';
    return description.str();
}

} // namespace revia::presentation
