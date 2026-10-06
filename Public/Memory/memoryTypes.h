#pragma once

#include "LLM/responseTypes.h"
#include "Memory/memoryScope.h"
#include <string>
#include <vector>

enum class memoryImportance
{
    Low,
    Medium,
    High
};

struct memoryEntry
{
    std::string id;
    std::string category;
    std::string summary;
    std::string source;
    std::string createdAt;
    std::string revisionChainId;
    std::string revisionReceiptId;
    std::string revisesMemoryId;
    std::string currentRevisionId;

    memoryImportance importance = memoryImportance::Medium;
    revia::memory::MemorySubject subject{};
};

struct memoryDecision
{
    bool bSuccess = false;
    bool bShouldRemember = false;
    // Yielded to an interactive turn without a verdict; bSuccess may still be true.
    // Preemption is distinct from deciding the turn is not worth remembering.
    bool bPreempted = false;

    std::string category;
    std::string summary;
    std::string reason;
    // Identifies how a preclassified memory entered the store. Ordinary conversation
    // remains "automatic"; sourced background findings use "autonomous_research".
    std::string source = "automatic";
    std::vector<float> embedding;
    std::string embeddingModel;
    std::vector<latencySample> timings;
    revia::memory::MemorySubject subject{};
};
