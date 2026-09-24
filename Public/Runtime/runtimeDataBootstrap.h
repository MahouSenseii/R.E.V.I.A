#pragma once

#include "Library/structLibrary.h"

#include <filesystem>
#include <string>

namespace revia::runtime
{

struct RuntimeDataBootstrapResult
{
    bool succeeded = false;
    bool defaultVoiceSeeded = false;
    std::string error;
};

// Creates the writable first-run directory layout and copies immutable starter
// assets without replacing anything the user has already created or selected.
RuntimeDataBootstrapResult BootstrapRuntimeData(
    const appSettings& settings,
    const std::filesystem::path& runtimeRoot = "RuntimeData",
    const std::filesystem::path& seedRoot = "Config/Defaults/RuntimeData");

}
