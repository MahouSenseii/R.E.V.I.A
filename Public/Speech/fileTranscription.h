#pragma once

#include "Speech/recognitionSettings.h"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::speech
{
struct TranscriptSegment
{
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
    std::string text;
};

struct FileTranscriptionResult
{
    bool ok = false;
    bool cancelled = false;
    std::vector<TranscriptSegment> segments;
    std::string error;
};

// Returns file-relative timestamps; the caller owns clip offsets, admission and the input file.
// An explicitly requested VAD must exist locally; failure never falls back to ungated ASR.
[[nodiscard]] FileTranscriptionResult TranscribeWaveFile(const std::filesystem::path& wavePath, const speechRecognitionSettings& settings,
    std::stop_token stopToken = {}, const std::filesystem::path& vadModelPath = {});
}
