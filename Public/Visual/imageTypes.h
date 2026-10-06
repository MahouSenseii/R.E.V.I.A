#pragma once

#include "Visual/imageArtifact.h"

#include <filesystem>
#include <string>

namespace revia::visual
{
struct ImageResult
{
    bool succeeded = false;
    bool cancelled = false;
    std::filesystem::path path;
    std::string message;
    std::string detail;
    double elapsedMilliseconds = 0.0;
    double modelLoadMilliseconds = 0.0;
    double inferenceMilliseconds = 0.0;
    double peakAllocatedMiB = 0.0;
    double peakReservedMiB = 0.0;
    ImageArtifactReceipt receipt;
};
}
