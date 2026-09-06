#pragma once

namespace revia::llm
{
// Per-request restriction on private curated memory. Denied suppresses retrieval,
// query embedding and classification without changing the active profile. Other
// context (channel history, screen data, posture) remains the caller's responsibility.
enum class PrivateMemoryAccess
{
    ProfileSetting,
    Denied
};
} // namespace revia::llm
