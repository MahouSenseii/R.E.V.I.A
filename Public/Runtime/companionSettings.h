#pragma once

#include "Core/appSettings.h"
#include <filesystem>

namespace revia::runtime
{
class CompanionPaths;

void BindCompanionSettings(appSettings& settings, const CompanionPaths& paths);
[[nodiscard]] std::filesystem::path CompanionLogDirectory(const CompanionPaths& paths);
}
