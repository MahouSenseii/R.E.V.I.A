#include "Identity/socialIdentity.h"

namespace revia::identity
{

std::string ToString(const AudienceKind audience)
{
    switch (audience)
    {
    case AudienceKind::Private:
        return "Private";
    case AudienceKind::Shared:
        return "Shared";
    case AudienceKind::Public:
        return "Public";
    default:
        return "Unknown";
    }
}

std::string ToString(const SpeakerSource source)
{
    switch (source)
    {
    case SpeakerSource::ExplicitIntroduction:
        return "Explicit introduction";
    case SpeakerSource::Platform:
        return "Platform";
    case SpeakerSource::ConsentedVoice:
        return "Consented voice";
    default:
        return "Unknown";
    }
}

} // namespace revia::identity
