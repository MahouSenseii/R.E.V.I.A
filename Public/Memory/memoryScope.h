#pragma once

#include "Identity/socialIdentity.h"

#include <cstdint>
#include <string>

namespace revia::memory
{
enum class MemorySubjectKind
{
    Unattributed,
    Participant,
    Companion
};

struct MemorySubject
{
    MemorySubjectKind kind = MemorySubjectKind::Unattributed;
    std::string entityId;
    bool operator==(const MemorySubject&) const = default;
};

// Captured by the host; stored provenance never grants current disclosure permission.
struct MemoryScope
{
    std::string participantId;
    identity::AudienceContext audience;
    identity::SpeakerSource participantSource = identity::SpeakerSource::Unknown;
    std::uint64_t consentRevision = 0;
    std::string companionId;
};

[[nodiscard]] bool IsValidSubject(const MemorySubject& subject);
[[nodiscard]] bool IsAttributedPrivateScope(const MemoryScope& scope);
[[nodiscard]] MemorySubject ParticipantSubject(const MemoryScope& scope);
[[nodiscard]] std::string SubjectKey(const MemorySubject& subject);
}
