#pragma once

#include "Actions/actionTypes.h"
#include "Memory/memoryScope.h"

#include <filesystem>
#include <mutex>
#include <memory>
#include <optional>
#include "Audit/evidenceJournal.h"

namespace revia::audit
{

class ActionAuditLogger
{
  public:
    explicit ActionAuditLogger(std::filesystem::path path);
    explicit ActionAuditLogger(std::shared_ptr<EvidenceJournal> journal);

    [[nodiscard]] bool RecordIntent(
        const actions::ActionRequest& request, const actions::PolicyDecision& decision, const std::string& transactionId);

    [[nodiscard]] bool Record(const actions::ActionRequest& request, const actions::PolicyDecision& decision,
        const actions::ActionResult& result, double elapsedMilliseconds = -1.0, const std::string& transactionId = {});

    [[nodiscard]] const std::filesystem::path& Path() const;
    [[nodiscard]] std::shared_ptr<EvidenceJournal> Journal() const
    {
        return journal;
    }
    void SetScope(memory::MemoryScope scope);

  private:
    [[nodiscard]] bool WriteRecord(const actions::ActionRequest& request, const actions::PolicyDecision& decision,
        const actions::ActionResult& result, double elapsedMilliseconds, const char* recordType, const std::string& transactionId);

    std::filesystem::path path;
    std::mutex mutex;
    std::shared_ptr<EvidenceJournal> journal;
    std::optional<memory::MemoryScope> scope;
};

} // namespace revia::audit
