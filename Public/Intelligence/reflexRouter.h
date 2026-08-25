#pragma once

#include "Intelligence/intelligenceTypes.h"
#include "Runtime/affectTypes.h"

#include <cstddef>
#include <string>

namespace revia::intelligence
{

struct ReflexContext
{
    runtime::AffectSnapshot affect;
    bool busy = false;
    bool interruptedGeneration = false;
    std::size_t repeatedCalls = 0;
    std::string previousResponse;
};

struct ReflexResult
{
    bool matched = false;
    bool requestsCancellation = false;
    bool shouldSpeak = true;
    std::string response;
    std::string reason;
};

class ReflexRouter
{
public:
    [[nodiscard]] ReflexResult Route(
        const std::string& input,
        const ReflexContext& context) const;
};

} // namespace revia::intelligence
