#include "Intelligence/intelligenceTypes.h"

namespace revia::intelligence
{

std::string ToString(const IntelligenceTier tier)
{
    switch (tier)
    {
        case IntelligenceTier::Reflex: return "Reflex";
        case IntelligenceTier::Fast: return "Fast";
        case IntelligenceTier::Main: return "Main";
        case IntelligenceTier::Expert: return "Expert";
        case IntelligenceTier::Vision: return "Vision";
        case IntelligenceTier::ExpertVision: return "ExpertVision";
        default: return "Main";
    }
}

std::string ToString(const ReasoningMode mode)
{
    return mode == ReasoningMode::Deep ? "Deep" : "Fast";
}

} // namespace revia::intelligence
