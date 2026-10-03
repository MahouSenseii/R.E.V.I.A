#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace revia::speech
{

enum class SystemCueKind
{
    Filter,
    Warning,
    Error,
    Stopped,
    Degraded,
    NeedsAttention,
    VoiceFailed
};

struct SystemCueDefinition
{
    SystemCueKind kind;
    std::string_view id;
    std::string_view phrase;
};

inline constexpr unsigned SystemCuePhraseVersion = 1;
[[nodiscard]] const std::array<SystemCueDefinition, 7>& ApprovedSystemCues();

enum class SystemCuePhase
{
    Unavailable,
    Preparing,
    Prepared,
    PreparationFailed,
    Queued,
    Playing,
    Played,
    Cancelled,
    PlaybackFailed
};

struct SystemCueSnapshot
{
    SystemCuePhase phase = SystemCuePhase::Unavailable;
    std::size_t readyClips = 0;
    std::size_t totalClips = 7;
};

} // namespace revia::speech
