#pragma once

#include <filesystem>
#include <string>

namespace revia::core
{

// The install's local API bearer token, kept at `path` and created on first use with
// GenerateLocalApiKey. On Windows the file holds it encrypted with DPAPI for the current
// user, so another account or a copied file cannot read it; elsewhere it is plain text
// readable only by its owner. A file that cannot be read back is replaced with a new
// token. Empty with `error` set when no token can be read or saved.
[[nodiscard]] std::string LoadOrCreateLocalApiToken(const std::filesystem::path& path, std::string& error);

// True for a token GenerateLocalApiKey could have produced.
[[nodiscard]] bool IsLocalApiToken(const std::string& token);

} // namespace revia::core
