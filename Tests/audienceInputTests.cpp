#include "Agents/inputArbiter.h"
#include "testSupport.h"

#include <chrono>
#include <iostream>

void RunAudienceInputTests()
{
    using revia::agents::InputSource;
    using revia::agents::InputVerdict;
    using revia::tests::Check;
    const auto now = std::chrono::system_clock::now();
    revia::agents::InputContext first;
    first.stamp = {"companion-a", "session-a", 1};
    first.audience = {revia::identity::AudienceKind::Unknown, "room", 1, {}};
    first.participantId = "speaker-a";
    auto second = first;
    second.participantId = "speaker-b";
    revia::agents::InputArbiter arbiter;
    Check(arbiter.Offer("Revia help me plan the task", InputSource::Voice, now, first) == InputVerdict::Queued &&
        arbiter.Offer("Revia help me plan the task", InputSource::Voice, now, second) == InputVerdict::Queued,
        "Duplicate suppression merged distinct current participants.");
    const auto mixed = arbiter.TakeBatch();
    Check(!mixed.contextMatched && mixed.text.empty() && arbiter.Size() == 0,
        "A mixed-participant burst became one trusted conversation turn.");
    Check(arbiter.Offer("Revia explain the plan", InputSource::Voice, now, first) == InputVerdict::Queued,
        "Fresh participant input was not admitted.");
    Check(arbiter.Offer("and the next step", InputSource::Voice, now, first) == InputVerdict::Queued,
        "Same-participant continuation was not admitted.");
    const auto joined = arbiter.TakeBatch();
    Check(joined.contextMatched && joined.context.participantId == first.participantId &&
        joined.text == "Revia explain the plan. and the next step",
        "A legitimate same-audience burst lost its captured context.");
    Check(arbiter.Offer("First private fragment", InputSource::Typed, now, first) == InputVerdict::Queued,
        "First privacy-bound input was not queued.");
    ++second.audience.revision;
    second.participantId = first.participantId;
    Check(arbiter.Offer("Later room fragment", InputSource::Voice, now, second) == InputVerdict::Queued,
        "Later room input was not queued for context rejection.");
    Check(!arbiter.TakeBatch().contextMatched, "An audience revision change was merged with a private fragment.");
    std::cout << "Captured participant/audience input checks passed.\n";
}
