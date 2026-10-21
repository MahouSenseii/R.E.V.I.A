#pragma once

#include "Memory/memoryTypes.h"
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

class longTermMemory;

namespace revia::memory
{
// A review suggestion, never authorization. Acceptance must use the existing
// owner-revision transaction with exact original and replacement guards.
struct MemoryCorrectionProposal
{
    std::string id;
    memoryEntry original;
    std::string expectedSummaryDigest;
    std::string proposedMemoryId;
    memoryDecision corrected;
    std::string evidence;
    MemoryScope scope;
    std::uint64_t turnId = 0;
};

// At most three current same-subject/category candidates from the existing
// bounded retrieval owner. No writes, model calls, or correction authority.
[[nodiscard]] std::vector<MemoryCorrectionProposal> ProposeMemoryCorrections(
    const longTermMemory& store, const std::string& savedMemoryId, const memoryDecision& savedDecision,
    const std::string& userInput, const MemoryScope& scope, std::uint64_t turnId,
    std::function<bool()> admission = {}, std::stop_token stopToken = {});
} // namespace revia::memory
