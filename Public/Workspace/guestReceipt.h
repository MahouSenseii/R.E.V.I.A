#pragma once

#include "Workspace/guestTypes.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace revia::workspace
{

// The guest worker's own bounds on files a sandboxed run sends back.
inline constexpr std::size_t MaxGuestArtifacts = 4;
inline constexpr std::size_t MaxGuestArtifactBytes = 524288;

// Strict standard base64 with padding; false for anything else.
[[nodiscard]] bool DecodeBase64(const std::string& text, std::string& bytes);

// The "artifacts" of a run_python receipt: each decoded, sized and checked against its
// SHA-256, within the count and total bounds. Throws std::runtime_error("invalid_receipt")
// on anything malformed, so a bad receipt is never half believed.
[[nodiscard]] std::vector<GuestArtifact> ParseGuestArtifacts(const nlohmann::json& receipt);

} // namespace revia::workspace
