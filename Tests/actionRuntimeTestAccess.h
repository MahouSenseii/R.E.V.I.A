#pragma once

#include "Actions/actionRuntime.h"
#include "Internet/visibleBrowserClient.h"

namespace revia::actions
{

struct ActionRuntimeTestAccess
{
    static std::shared_ptr<internet::VisibleBrowserCancellation> BrowserCancellation(ActionRuntime& runtime)
    { return runtime.internetCancellation; }
};

} // namespace revia::actions
