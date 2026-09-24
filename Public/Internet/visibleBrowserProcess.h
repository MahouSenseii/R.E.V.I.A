#pragma once

#include "Actions/actionTypes.h"

#include <string>

namespace revia::actions::internet
{

// Owns only the project-supplied browser worker and its descendants. Browser policy and
// response parsing stay outside this class so process lifecycle remains independently
// testable and an existing personal browser can never be adopted accidentally.
class VisibleBrowserProcess
{
public:
    VisibleBrowserProcess() = default;
    ~VisibleBrowserProcess();

    VisibleBrowserProcess(const VisibleBrowserProcess&) = delete;
    VisibleBrowserProcess& operator=(const VisibleBrowserProcess&) = delete;

    [[nodiscard]] bool Start(
        const CapabilitySettings::InternetAccess& settings,
        std::string& outError);
    [[nodiscard]] bool IsRunning() const;
    void Stop();

    [[nodiscard]] int Port() const;
    [[nodiscard]] const std::string& Token() const;

private:
    void* processHandle = nullptr;
    void* jobHandle = nullptr;
    int port = 0;
    std::string token;
};

} // namespace revia::actions::internet
