#pragma once

#include <string>
#include <vector>

struct speechRecognitionSettings
{
    bool bEnabled = true;
    std::string executable = "ThirdParty/whisper/whisper-cli.exe";
    bool bUseServer = true;
    std::string serverExecutable = "ThirdParty/whisper/whisper-server.exe";
    std::string serverHost = "127.0.0.1";
    int serverPort = 8094;
    int serverStartupTimeoutSeconds = 60;
    int requestTimeoutSeconds = 180;
    std::string modelPath = "Models/ggml-small.en.bin";
    std::string language = "en";
    int sampleRate = 16000;
    int threads = 6;
    bool bUseGpu = true;
    // "cpu", "auto", or "cuda:N". `useGpu` remains as the backward-compatible
    // coarse switch; the startup resource plan resolves auto to one exact device.
    std::string device = "auto";
    // Hands-free capture admits voiced segments through the input arbiter without granting authority.
    bool bHandsFree = false;
    // Select by product name because device ordinals change when hardware changes.
    // Empty or "Default" follows Windows; unavailable names produce an error.
    std::string microphoneDevice;
    int vadEnergyThreshold = 900;
    int vadSpeechFrames = 3;
    int vadSilenceMs = 350;
    int minimumUtteranceMs = 350;
    int maximumUtteranceSeconds = 24;
    // Hands-free answers only speech that names her or follows up on a conversation
    // with her within followUpSeconds. False answers everything the microphone hears.
    bool bRequireWakeWord = true;
    std::vector<std::string> wakeWords = {"revia", "rivia", "revya", "reviya", "revea"};
    int followUpSeconds = 20;
};
