#pragma once

#include "Core/evidenceRef.h"

#include <optional>
#include <utility>

namespace revia::core
{
class EvidenceBundle
{
  public:
    EvidenceBundle(const EvidenceBundle&) = default;
    EvidenceBundle(EvidenceBundle&&) = default;
    [[nodiscard]] const std::vector<EvidenceRef>& references() const
    {
        return references_;
    }
    [[nodiscard]] const runtime::RuntimeStamp& stamp() const
    {
        return stamp_;
    }
    [[nodiscard]] const memory::MemoryScope& scope() const
    {
        return scope_;
    }
    [[nodiscard]] const SchemaVersion& version() const
    {
        return version_;
    }

    [[nodiscard]] static std::optional<EvidenceBundle> Create(std::vector<EvidenceRef> references, runtime::RuntimeStamp stamp,
        memory::MemoryScope scope, ContractValidation& validation, SchemaVersion version = {});

  private:
    EvidenceBundle(std::vector<EvidenceRef> references, runtime::RuntimeStamp stamp, memory::MemoryScope scope, SchemaVersion version)
        : references_(std::move(references)), stamp_(std::move(stamp)), scope_(std::move(scope)), version_(version)
    {
    }

    const std::vector<EvidenceRef> references_;
    const runtime::RuntimeStamp stamp_;
    const memory::MemoryScope scope_;
    const SchemaVersion version_;
};

[[nodiscard]] ContractValidation ValidateEvidenceAdmission(const EvidenceBundle& bundle, const TaskContract& task,
    const runtime::RuntimeStamp& current, const memory::MemoryScope& scope, const CurrentStampGuard& matchesCurrent,
    std::stop_token cancellation = {});
[[nodiscard]] ContractValidation SerializeEvidenceBundle(const EvidenceBundle& bundle, std::string& output);
[[nodiscard]] std::optional<EvidenceBundle> DeserializeEvidenceBundle(std::string_view json, ContractValidation& validation);
}
