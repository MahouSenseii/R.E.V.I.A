#include "Runtime/sessionIdentity.h"
#include "Runtime/runtimeEvents.h"

#include <iostream>
#include <vector>

int main()
{
    using namespace revia::runtime;
    int failures = 0;
    auto check = [&](bool condition, const char* name)
    {
        std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
        if (!condition)
            ++failures;
    };
    SessionIdentity a("companion-a");
    SessionIdentity b("companion-b");
    RuntimeEventBus bus;
    const RuntimeStamp first = a.Stamp("task-1", "attempt-1", 7);
    bus.BindOrigin(first, [&](const RuntimeStamp& stamp) { return a.IsCurrent(stamp); });
    std::vector<RuntimeEvent> delivered;
    bus.Subscribe([&](const RuntimeEvent& event) { delivered.push_back(event); });
    bus.Publish(RuntimeEvent{RuntimeEventKind::Memory, RuntimeState::Remembering, "a-memory"});
    check(delivered.size() == 1 && delivered.front().stamp.SameSession(first), "bus captures originating session");
    RuntimeEvent foreign{RuntimeEventKind::AssistantMessage, RuntimeState::Idle, "b-result"};
    foreign.stamp = b.Stamp();
    bus.Publish(foreign);
    check(delivered.size() == 1, "foreign companion cannot publish through A bus");
    a.Invalidate();
    bus.Publish(RuntimeEvent{RuntimeEventKind::Memory, RuntimeState::Remembering, "late-a-memory"});
    check(delivered.size() == 1 && !a.IsCurrent(first), "invalidated lifetime rejects delayed publication");
    a.BeginSession();
    const RuntimeStamp returned = a.Stamp();
    check(returned.companionId == first.companionId && returned.sessionId != first.sessionId && returned.generation > first.generation,
        "returning to A creates a new session generation");
    check(!a.IsCurrent(first) && a.IsCurrent(returned), "A to B to A cannot readmit old A work");
    bus.BindOrigin(returned, [&](const RuntimeStamp& stamp) { return a.IsCurrent(stamp); });
    RuntimeEvent old{RuntimeEventKind::AssistantMessage, RuntimeState::Idle, "old-a"};
    old.stamp = first;
    bus.Publish(old);
    bus.Publish(RuntimeEvent{RuntimeEventKind::AssistantMessage, RuntimeState::Idle, "new-a"});
    check(delivered.size() == 2 && delivered.back().message == "new-a", "new generation receives only current work");
    check(b.IsCurrent(b.Stamp()) && !b.IsCurrent(returned), "simultaneous companion admission remains independent");
    return failures == 0 ? 0 : 1;
}
