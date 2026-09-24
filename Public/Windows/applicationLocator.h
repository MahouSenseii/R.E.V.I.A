#pragma once

#include <string>

namespace revia::actions::windows
{

// Resolves an executable basename through Windows search paths and App Paths.
// Returns a file path only; never interprets command lines or launches a process.
[[nodiscard]] std::wstring ResolveApplicationExecutable(const std::wstring& name);

} // namespace revia::actions::windows
