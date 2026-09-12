#include "Presentation/presentationBus.h"

#include <algorithm>
#include <utility>

namespace revia::presentation
{

std::string ToString(const PresentationEventKind kind)
{
    switch (kind)
    {
        case PresentationEventKind::EmotionChanged: return "EmotionChanged";
        case PresentationEventKind::MoodChanged: return "MoodChanged";
        case PresentationEventKind::StartedListening: return "StartedListening";
        case PresentationEventKind::StoppedListening: return "StoppedListening";
        case PresentationEventKind::StartedThinking: return "StartedThinking";
        case PresentationEventKind::StoppedThinking: return "StoppedThinking";
        case PresentationEventKind::StartedSpeaking: return "StartedSpeaking";
        case PresentationEventKind::StoppedSpeaking: return "StoppedSpeaking";
        case PresentationEventKind::SpeechInterrupted: return "SpeechInterrupted";
        case PresentationEventKind::StartedResearching: return "StartedResearching";
        case PresentationEventKind::StoppedResearching: return "StoppedResearching";
        case PresentationEventKind::StartedComputerTask: return "StartedComputerTask";
        case PresentationEventKind::StoppedComputerTask: return "StoppedComputerTask";
        case PresentationEventKind::StartedSinging: return "StartedSinging";
        case PresentationEventKind::StoppedSinging: return "StoppedSinging";
        case PresentationEventKind::VocalStarted: return "VocalStarted";
        case PresentationEventKind::VocalEnded: return "VocalEnded";
        case PresentationEventKind::CuriosityRaised: return "CuriosityRaised";
        case PresentationEventKind::BecameIdle: return "BecameIdle";
        case PresentationEventKind::UserInterrupted: return "UserInterrupted";
        case PresentationEventKind::GoalCompleted: return "GoalCompleted";
        case PresentationEventKind::GoalFailed: return "GoalFailed";
    }
    return "Unknown";
}

std::string SanitizeSubject(const std::string& value)
{
    std::string trimmed;
    trimmed.reserve(std::min(value.size(), PresentationSubjectLimit));
    for (const char character : value)
    {
        if (trimmed.size() >= PresentationSubjectLimit) break;
        // A newline is the first thing a multi-paragraph payload brings with it, and a
        // label has no use for one.
        trimmed.push_back(
            character == '\n' || character == '\r' || character == '\t' ? ' ' : character);
    }
    while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
    return trimmed;
}

PresentationBus::SubscriptionId PresentationBus::Add(std::shared_ptr<IPresentationSink> sink)
{
    if (sink == nullptr) return 0;
    const std::lock_guard<std::mutex> lock(mutex);
    const SubscriptionId id = nextId++;
    sinks.emplace_back(id, std::move(sink));
    return id;
}

void PresentationBus::Remove(const SubscriptionId id)
{
    const std::lock_guard<std::mutex> lock(mutex);
    sinks.erase(
        std::remove_if(sinks.begin(), sinks.end(),
            [id](const auto& entry) { return entry.first == id; }),
        sinks.end());
}

void PresentationBus::Publish(PresentationEvent event)
{
    std::vector<std::shared_ptr<IPresentationSink>> targets;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        event.sequence = ++sequence;
        event.occurredAt = std::chrono::steady_clock::now();
        // Applied here rather than trusted from the publisher. A single choke point is
        // the only version of this rule that cannot be forgotten at a new call site.
        event.subject = SanitizeSubject(event.subject);
        targets.reserve(sinks.size());
        for (const auto& entry : sinks) targets.push_back(entry.second);
    }
    // Fanned out with the lock released: a sink that draws something must not be able to
    // stall every other sink, and must not deadlock by touching the bus.
    for (const std::shared_ptr<IPresentationSink>& sink : targets)
    {
        if (sink != nullptr) sink->OnPresentation(event);
    }
}

std::vector<std::string> PresentationBus::SinkNames() const
{
    const std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> names;
    names.reserve(sinks.size());
    for (const auto& entry : sinks)
    {
        if (entry.second != nullptr) names.push_back(entry.second->Name());
    }
    return names;
}

std::uint64_t PresentationBus::Published() const
{
    const std::lock_guard<std::mutex> lock(mutex);
    return sequence;
}

std::optional<PresentationEvent> TranslateRuntimeEvent(const runtime::RuntimeEvent& event)
{
    using runtime::RuntimeEventKind;
    using runtime::RuntimeState;

    PresentationEvent translated;
    translated.affect = event.affect;
    translated.affectIntensity = event.affectIntensity;

    switch (event.kind)
    {
        // Her private reasoning. There is deliberately no presentation event for it: a
        // renderer showing "she is thinking" is a state, and a renderer showing what she
        // is thinking is a leak. Dropped here rather than filtered downstream, so a new
        // sink cannot subscribe its way around the rule.
        case RuntimeEventKind::SelfInquiry:
            return std::nullopt;

        // A round of investigation is a visible *state* -- she is working -- and never
        // its content. The renderer learns that she is checking something; it does not
        // learn what, because that is the same leak SelfInquiry is dropped for.
        case RuntimeEventKind::InvestigationChecking:
            translated.kind = PresentationEventKind::StartedThinking;
            return translated;
        case RuntimeEventKind::InvestigationFindings:
            translated.kind = PresentationEventKind::StoppedThinking;
            return translated;

        case RuntimeEventKind::AffectChanged:
            translated.kind = PresentationEventKind::EmotionChanged;
            return translated;

        case RuntimeEventKind::Performance:
        {
            // `phase` carries the performance event kind and `detail` the karaoke line.
            // The line is lyrics, not state, and does not cross.
            if (event.phase == "SongStarted")
            {
                translated.kind = PresentationEventKind::StartedSinging;
                translated.subject = event.message;
                return translated;
            }
            if (event.phase == "SongEnded" || event.phase == "SongInterrupted" ||
                event.phase == "SongFailed")
            {
                translated.kind = PresentationEventKind::StoppedSinging;
                return translated;
            }
            if (event.phase == "VocalStarted")
            {
                translated.kind = PresentationEventKind::VocalStarted;
                return translated;
            }
            if (event.phase == "VocalEnded")
            {
                translated.kind = PresentationEventKind::VocalEnded;
                return translated;
            }
            return std::nullopt;
        }

        case RuntimeEventKind::StateChanged:
        {
            switch (event.state)
            {
                case RuntimeState::Thinking:
                    translated.kind = PresentationEventKind::StartedThinking;
                    return translated;
                case RuntimeState::Responding:
                    translated.kind = PresentationEventKind::StartedSpeaking;
                    return translated;
                case RuntimeState::Acting:
                    translated.kind = PresentationEventKind::StartedComputerTask;
                    return translated;
                case RuntimeState::Idle:
                    translated.kind = PresentationEventKind::BecameIdle;
                    return translated;
                default:
                    return std::nullopt;
            }
        }

        default:
            return std::nullopt;
    }
}

} // namespace revia::presentation
