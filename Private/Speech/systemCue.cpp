#include "Speech/systemCue.h"

namespace revia::speech
{

const std::array<SystemCueDefinition, 7>& ApprovedSystemCues()
{
    static constexpr std::array<SystemCueDefinition, 7> catalog = {
        {{SystemCueKind::Filter, "filter", "Filter."}, {SystemCueKind::Warning, "warning", "Warning."},
            {SystemCueKind::Error, "error", "Error."}, {SystemCueKind::Stopped, "stopped", "Stopped."},
            {SystemCueKind::Degraded, "degraded", "Degraded."}, {SystemCueKind::NeedsAttention, "needs_attention", "Needs attention."},
            {SystemCueKind::VoiceFailed, "voice_failed", "Voice failed."}}};
    return catalog;
}

} // namespace revia::speech
