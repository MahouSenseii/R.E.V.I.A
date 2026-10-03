#pragma once
#include "Speech/speechRecognitionService.h"
namespace revia::speech
{
struct SpeakerRecognitionTestAccess
{
    static identity::SpeakerObservation Resolve(
        SpeechRecognitionService& service, const std::filesystem::path& wave, std::stop_token stop = {})
    {
        return service.ResolveSpeaker(wave, stop);
    }
};
}
