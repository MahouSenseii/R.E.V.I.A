#include "Audit/runtimeEvidenceBridge.h"
#include "Audit/contentDigest.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

void RunRuntimeEvidenceBridgeTests()
{
    using namespace revia;
    const auto directory = std::filesystem::temp_directory_path() / ("revia-event-journal-" + audit::NewJournalEventId());
    std::filesystem::create_directories(directory);
    const auto path = directory / "actions.jsonl";
    auto journal = std::make_shared<audit::EvidenceJournal>(path);
    runtime::RuntimeEventBus bus;
    const runtime::RuntimeStamp origin{"bridge-companion", "bridge-session", 1, "", "", 2};
    bus.BindOrigin(origin, [](const auto&) { return true; });
    audit::RuntimeEvidenceBridge bridge;
    bridge.Start(bus, journal);
    bus.Publish({runtime::RuntimeEventKind::StateChanged, runtime::RuntimeState::Thinking, "Secret conversation text"});
    bus.Publish({runtime::RuntimeEventKind::AssistantMessage, runtime::RuntimeState::Responding, "Private reply omitted"});
    bridge.Stop();
    std::ifstream input(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    if (bytes.find("Secret conversation text") != std::string::npos || bytes.find("Private reply omitted") != std::string::npos)
        throw std::runtime_error("Runtime telemetry disclosed conversation content.");
    auto stamp = origin;
    stamp.taskId = "runtime-observations";
    stamp.attemptId = "lifecycle";
    memory::MemoryScope scope;
    scope.companionId = origin.companionId;
    scope.audience = {identity::AudienceKind::Unknown, "runtime-system", 0, {}};
    const auto evidence = journal->Read({stamp, scope, {}, {audit::JournalKind::Observation}});
    if (evidence.size() != 1 || evidence.front().sourceId != "runtime-lifecycle" || evidence.front().observedAtUnixMs == 0)
        throw std::runtime_error("Runtime lifecycle did not retain original provenance in the shared journal.");
    bus.Publish({runtime::RuntimeEventKind::StateChanged, runtime::RuntimeState::Idle, "After stop"});
    if (journal->Read({stamp, scope, {}, {}}).size() != 1)
        throw std::runtime_error("Stopped runtime bridge still recorded events.");
    std::filesystem::remove_all(directory);
    std::cout << "Runtime shared journal lifecycle, redaction, provenance and shutdown passed.\n";
}
