#include "testSupport.h"

#include "Presentation/avatarState.h"
#include "Presentation/debugPresentationSink.h"
#include "Presentation/presentationBus.h"
#include "Speech/speechCoordinator.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{

using revia::tests::Check;

// What the orchestration itself costs.
//
// Deliberately narrow. This measures the decision -- routing an intent, fanning an event
// to its sinks -- and nothing else. It is not a speech latency measurement and must not
// be read as one: no model runs here, no audio is synthesised, and nothing is played.
// How long it takes Revia to answer out loud is an end-to-end question that this cannot
// and does not answer.

struct Percentiles
{
    double median = 0.0;
    double p95 = 0.0;
    double worst = 0.0;
};

Percentiles Summarize(std::vector<double> samples)
{
    Percentiles result;
    if (samples.empty()) return result;
    std::sort(samples.begin(), samples.end());
    // Nearest-rank, stated plainly rather than left to a library's interpolation choice.
    const auto rank = [&samples](const double fraction)
    {
        const std::size_t index = static_cast<std::size_t>(
            fraction * static_cast<double>(samples.size()));
        return samples[std::min(index, samples.size() - 1)];
    };
    result.median = rank(0.5);
    result.p95 = rank(0.95);
    result.worst = samples.back();
    return result;
}

void Report(const std::string& what, const Percentiles& numbers, const double target)
{
    std::cout << "  " << what << ": median " << std::fixed << std::setprecision(4)
              << numbers.median << " ms, p95 " << numbers.p95 << " ms, worst "
              << numbers.worst << " ms (target < " << std::setprecision(1) << target
              << " ms typical)\n" << std::setprecision(6);
}

void MeasureIntentRouting()
{
    revia::speech::SpeechCoordinator coordinator;
    // A channel that does nothing, so what is measured is the decision rather than the
    // speech backend behind it.
    revia::speech::SpeechChannel channel;
    std::uint64_t started = 0;
    channel.speak = [&](const revia::speech::SpeechIntent&, const std::uint64_t)
    {
        ++started;
    };
    channel.stopSpeech = []() {};
    channel.stopSong = [](const std::string&) {};
    coordinator.SetChannel(std::move(channel));

    constexpr int Iterations = 2000;
    std::vector<double> samples;
    samples.reserve(Iterations);

    for (int index = 0; index < Iterations; ++index)
    {
        revia::speech::SpeechIntent intent;
        // Rotated so the measurement covers taking the floor, queueing behind it, and
        // being refused, rather than only the cheapest path.
        intent.owner = index % 3 == 0 ? revia::speech::SpeechOwner::Conversation
            : index % 3 == 1 ? revia::speech::SpeechOwner::Autonomy
            : revia::speech::SpeechOwner::System;
        intent.behavior = index % 2 == 0 ? revia::speech::SpeechBehavior::Queue
            : revia::speech::SpeechBehavior::Interrupt;
        intent.text = "a sentence of roughly ordinary length for a spoken reply";

        const auto before = std::chrono::steady_clock::now();
        const revia::speech::SpeechSubmission submission =
            coordinator.Submit(std::move(intent));
        const auto after = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(after - before).count());

        // Drain periodically so the queue does not grow without bound and turn this into
        // a measurement of how long a very long queue is.
        if (index % 8 == 7)
        {
            const revia::speech::SpeechChannelStatus status = coordinator.Status();
            if (status.speaking) coordinator.NotePlaybackFinished(status.activeId);
        }
        static_cast<void>(submission);
    }

    const Percentiles numbers = Summarize(samples);
    Report("speech intent routing", numbers, 5.0);
    Check(numbers.median < 5.0,
        "Routing one speech intent took longer than the 5 ms budget at the median.");
    Check(started > 0, "Nothing was ever started, so the measurement is of a no-op.");
}

void MeasurePresentationDelivery()
{
    revia::presentation::PresentationBus bus;
    auto avatar = std::make_shared<revia::presentation::PresentationController>();
    auto debug = std::make_shared<revia::presentation::DebugPresentationSink>();
    bus.Add(avatar);
    bus.Add(debug);

    constexpr int Iterations = 2000;
    std::vector<double> samples;
    samples.reserve(Iterations);

    const revia::presentation::PresentationEventKind rotation[]{
        revia::presentation::PresentationEventKind::StartedSpeaking,
        revia::presentation::PresentationEventKind::StoppedSpeaking,
        revia::presentation::PresentationEventKind::EmotionChanged,
        revia::presentation::PresentationEventKind::VocalStarted,
        revia::presentation::PresentationEventKind::VocalEnded};

    for (int index = 0; index < Iterations; ++index)
    {
        revia::presentation::PresentationEvent event;
        event.kind = rotation[index % 5];
        event.subject = "a label";
        event.affectIntensity = 0.5F;

        const auto before = std::chrono::steady_clock::now();
        bus.Publish(std::move(event));
        const auto after = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(after - before).count());
    }

    const Percentiles numbers = Summarize(samples);
    Report("presentation delivery (2 sinks)", numbers, 5.0);
    Check(numbers.median < 5.0,
        "Delivering one presentation event took longer than the 5 ms budget at the "
        "median.");
    Check(bus.Published() == Iterations, "The bus lost events while being measured.");
}

void MeasureTranslationCost()
{
    // The translation runs on every runtime event once the bus is wired, including the
    // many that produce nothing, so the cost of deciding "not visible" matters as much
    // as the cost of translating.
    constexpr int Iterations = 4000;
    std::vector<double> samples;
    samples.reserve(Iterations);
    std::size_t translated = 0;

    for (int index = 0; index < Iterations; ++index)
    {
        revia::runtime::RuntimeEvent event;
        event.kind = index % 4 == 0 ? revia::runtime::RuntimeEventKind::AffectChanged
            : index % 4 == 1 ? revia::runtime::RuntimeEventKind::SelfInquiry
            : index % 4 == 2 ? revia::runtime::RuntimeEventKind::Memory
            : revia::runtime::RuntimeEventKind::Performance;
        event.phase = "VocalStarted";
        event.message = "something";

        const auto before = std::chrono::steady_clock::now();
        const auto visible = revia::presentation::TranslateRuntimeEvent(event);
        const auto after = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(after - before).count());
        if (visible.has_value()) ++translated;
    }

    const Percentiles numbers = Summarize(samples);
    Report("runtime event translation", numbers, 5.0);
    Check(numbers.median < 5.0, "Translating a runtime event exceeded its budget.");
    // Half the events in the rotation are visible; the rest, including her private
    // reasoning, produce nothing at all.
    Check(translated == Iterations / 2,
        "The translation admitted a different set of events than expected.");
}

} // namespace

void RunCoordinationOverheadTests()
{
    std::cout << "Coordination overhead (orchestration only -- not speech latency):\n";
    MeasureIntentRouting();
    MeasurePresentationDelivery();
    MeasureTranslationCost();
}
