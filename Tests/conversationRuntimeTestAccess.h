#pragma once

#include "Runtime/conversationRuntime.h"

#include <utility>

namespace revia::runtime
{

// Replaces the collaborators a session wired, so a test drives the real turn with a
// scripted web lookup: the policy, the reader and the citations all run as they do in
// use, and only the network is a fixture.
struct ConversationRuntimeTestAccess
{
    static void SetInternetLookup(ConversationRuntime& runtime, ConversationRuntime::InternetLookupHandler handler)
    {
        runtime.internetLookup = std::move(handler);
    }
    static void SetInternetSettings(ConversationRuntime& runtime, ConversationRuntime::InternetSettingsProvider provider)
    {
        runtime.internetSettings = std::move(provider);
    }
    static const std::string& ChecksDescription(const ConversationRuntime& runtime)
    {
        return runtime.checksDescription;
    }
};

} // namespace revia::runtime
