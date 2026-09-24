#pragma once

#include <string>
#include <vector>

namespace revia::actions::windows
{

struct DiscoverableControl
{
    std::string name;
    std::string automationId;
    std::string permissionKey;
    int controlType = 0;
    bool supportsInvoke = false;
    bool supportsValue = false;
};

struct ApplicationControlInventory
{
    bool succeeded = false;
    std::string application;
    std::string windowTitle;
    std::vector<DiscoverableControl> controls;
    std::string reason;
};

// Read-only discovery for the permission editor. It inspects exactly the foreground
// HWND supplied by Windows and returns only enabled, visible controls that expose an
// Invoke or Value pattern. Discovery grants no permission by itself.
class ApplicationControlDiscovery
{
public:
    [[nodiscard]] ApplicationControlInventory InspectForeground(
        int maxControls = 500) const;
};

} // namespace revia::actions::windows
