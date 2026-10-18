#pragma once

#include "Actions/actionRuntime.h"
#include "Internet/visibleBrowserClient.h"

namespace revia::actions
{

struct ActionRuntimeTestAccess
{
    static void ReplaceExecutor(ActionRuntime& runtime, ActionType type, std::unique_ptr<IActionExecutor> executor)
    {
        std::lock_guard lock(runtime.mutex);
        runtime.dispatcher.Unregister(type);
        runtime.dispatcher.Register(std::move(executor));
    }

    static std::shared_ptr<internet::VisibleBrowserCancellation> BrowserCancellation(ActionRuntime& runtime)
    { return runtime.internetCancellation; }
};

} // namespace revia::actions
