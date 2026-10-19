#pragma once

#include <filesystem>
#include <string>

namespace revia::core
{

// Moves a fully written and checked temporary file over its target in one step, so a reader
// sees either the old file or the new one and never half of either. When the target already
// exists, its previous contents are kept beside it as "<target>.bak". The temporary file must
// sit in the target's directory. On failure the target is left as it was, the temporary file
// is removed, and outError says why. The one exception is a Windows swap that failed halfway
// and whose old file could not be moved back from the backup name: then the temporary file is
// kept too, so neither version is lost, and outError names both.
[[nodiscard]] bool ReplaceFileAtomically(
    const std::filesystem::path& target, const std::filesystem::path& temporary, std::string& outError);

} // namespace revia::core
