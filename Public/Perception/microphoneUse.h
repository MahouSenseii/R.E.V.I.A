#pragma once

#include <string>
#include <vector>

namespace revia::perception
{

struct MicrophoneUse
{
    // An executable name ("Zoom.exe") or a packaged app's family name.
    std::string application;
    // Using the microphone right now, not just at some point in the past.
    bool active = false;
};

// The apps Windows' privacy settings record as using the microphone, from the same
// registry data that drives the microphone icon in the taskbar. Empty elsewhere.
[[nodiscard]] std::vector<MicrophoneUse> ReadMicrophoneUse();

// Whether an app other than Revia is using the microphone now: in practice, a call or a
// meeting. Revia's own executables are ignored, because hands-free keeps the microphone
// open itself.
[[nodiscard]] bool OtherAppUsingMicrophone(
    const std::vector<MicrophoneUse>& uses,
    const std::vector<std::string>& ownApplications);

// ReadMicrophoneUse, judged against this process and Revia's known executables.
[[nodiscard]] bool InCall();

} // namespace revia::perception
