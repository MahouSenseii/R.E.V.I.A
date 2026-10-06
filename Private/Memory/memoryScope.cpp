#include "Memory/memoryScope.h"
#include "Core/utf8.h"

#include <algorithm>

namespace revia::memory
{
namespace
{
bool ValidId(const std::string& value)
{
    return !value.empty() && value.size() <= 128 && revia::utf8::IsValid(value) &&
           std::all_of(value.begin(), value.end(), [](const unsigned char ch) { return ch >= 0x20 && ch != 0x7f; });
}
}

bool IsValidSubject(const MemorySubject& subject)
{
    if (subject.kind == MemorySubjectKind::Unattributed)
        return subject.entityId.empty();
    if (subject.kind != MemorySubjectKind::Participant && subject.kind != MemorySubjectKind::Companion)
        return false;
    return ValidId(subject.entityId) && subject.entityId != "local:user" && !subject.entityId.starts_with("unknown:");
}

bool IsAttributedPrivateScope(const MemoryScope& scope)
{
    return ValidId(scope.companionId) && IsValidSubject({MemorySubjectKind::Participant, scope.participantId}) &&
           scope.audience.kind == identity::AudienceKind::Private && ValidId(scope.audience.audienceId) &&
           (scope.participantSource == identity::SpeakerSource::ExplicitIntroduction ||
               scope.participantSource == identity::SpeakerSource::Platform ||
               scope.participantSource == identity::SpeakerSource::ConsentedVoice) &&
           (scope.participantSource != identity::SpeakerSource::ConsentedVoice || scope.consentRevision != 0);
}

MemorySubject ParticipantSubject(const MemoryScope& scope)
{
    return IsAttributedPrivateScope(scope) ? MemorySubject{MemorySubjectKind::Participant, scope.participantId} : MemorySubject{};
}

std::string SubjectKey(const MemorySubject& subject)
{
    return std::to_string(static_cast<int>(subject.kind)) + ":" + std::to_string(subject.entityId.size()) + ":" + subject.entityId;
}
}
