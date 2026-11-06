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
    // Both rows are versions of the same fact slot (same person, attribute and object), so the model's
    // category labels for them may differ ("identity" for the original, "other" for the correction).
    bool sameFactSlot = false;
};

// At most three current same-subject/category candidates from the existing
// bounded retrieval owner. No writes, model calls, or correction authority.
[[nodiscard]] std::vector<MemoryCorrectionProposal> ProposeMemoryCorrections(
    const longTermMemory& store, const std::string& savedMemoryId, const memoryDecision& savedDecision,
    const std::string& userInput, const MemoryScope& scope, std::uint64_t turnId,
    std::function<bool()> admission = {}, std::stop_token stopToken = {});

// Review proposals rebuilt from stored fact slots: one per slot whose newest value is a clear,
// still-unreviewed correction or change, pairing it with the value it replaced. Read-only and
// derived from the database, so a restart loses nothing. Acceptance still needs the owner.
[[nodiscard]] std::vector<MemoryCorrectionProposal> ProposeFactCorrections(const longTermMemory& store, const MemoryScope& scope);
} // namespace revia::memory
