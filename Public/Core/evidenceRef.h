#pragma once

#include "Core/taskContract.h"

namespace revia::core
{
// Locators and content identity are provenance, never an independent truth verdict.
struct EvidenceRef
{
    SchemaVersion version;
    std::string id;
    std::string sourceLocator;
    std::string digest;
    std::string mediaType;
    runtime::RuntimeStamp stamp;
    memory::MemoryScope scope;
    std::uint64_t observedAtUnixMs = 0;
    std::string sourceId;
};

[[nodiscard]] ContractValidation ValidateEvidenceRef(const EvidenceRef& reference);
[[nodiscard]] ContractValidation SerializeEvidenceRef(const EvidenceRef& reference, std::string& output);
[[nodiscard]] ContractValidation DeserializeEvidenceRef(std::string_view json, EvidenceRef& output);
}
