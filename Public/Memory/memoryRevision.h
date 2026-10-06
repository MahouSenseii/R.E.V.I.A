#pragma once

#include "Memory/memoryTypes.h"
#include "Runtime/runtimeStamp.h"

#include <cstdint>
#include <string>

namespace revia::memory
{
// Only an explicit owner request identifies the original; classification and
// similarity cannot supply a revision target or reuse an earlier authorization.
struct MemoryRevisionRequest
{
    std::string ownerRequestId;
    std::string originalId;
    std::string expectedSummaryDigest;
    std::string priorReceiptId;
    runtime::RuntimeStamp origin;
    std::uint64_t audienceRevision = 0;
    memoryDecision corrected;
    std::string reason;
    std::string evidence;
    MemorySubject expectedSubject{};
};

struct MemoryRevisionReceipt
{
    std::string requestId;
    std::string digest;
    std::string originalId;
    std::string revisedId;
    std::string chainId;
    std::string priorReceiptId;
    runtime::RuntimeStamp origin;
    std::uint64_t audienceRevision = 0;
    std::string reason;
    std::string evidence;
    std::string createdAt;
    bool wasAdded = false;
    MemorySubject subject{};
};
} // namespace revia::memory
