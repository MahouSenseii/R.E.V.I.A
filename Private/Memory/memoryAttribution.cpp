#include "Memory/memoryAttribution.h"

namespace revia::memory
{

bool IsSelfMemoryCategory(const std::string& category)
{
    return category == "self_preference" || category == "self_relationship" ||
        category == "self_opinion";
}

bool AttributableToRevia(
    const agents::ResponseProvenance provenance,
    const std::string& category)
{
    // A memory about the user is unaffected by whose voice the reply was in. "Repeat
    // exactly: I hate jazz." still tells us the user asked for a repetition, and if
    // something in that exchange is worth recording about them, it is.
    if (!IsSelfMemoryCategory(category)) return true;
    return agents::MayExpressOwnOpinion(provenance);
}

} // namespace revia::memory
