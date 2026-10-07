#pragma once

#include "Audit/evidenceJournal.h"
#include "Runtime/runtimeEvents.h"

#include <condition_variable>
#include <memory>
#include <thread>

namespace revia::audit
{
// Lifecycle metadata uses an explicit system scope. Conversation content remains with its captured producer.
class RuntimeEvidenceBridge
{
  public:
    using DiagnosticSink = std::function<void(const JournalHealth&)>;
    RuntimeEvidenceBridge() = default;
    ~RuntimeEvidenceBridge();
    void Start(runtime::RuntimeEventBus& bus, std::shared_ptr<EvidenceJournal> journal, DiagnosticSink diagnostic = {});
    void Stop();

  private:
    struct State;
    std::shared_ptr<State> state;
    runtime::RuntimeEventBus* bus = nullptr;
    runtime::RuntimeEventBus::SubscriptionId subscription = 0;
    std::jthread worker;
};
}
