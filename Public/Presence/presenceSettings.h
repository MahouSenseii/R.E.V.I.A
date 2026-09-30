#pragma once

#include <string>
#include <vector>

// Local presentation and input-routing files; no inference, memory or action ownership.
struct presenceSettings
{
    bool bEnabled = true;
    bool bAvatarBridgeEnabled = true;
    std::string statePath = "RuntimeData/Presence/avatar_state.json";
    std::string eventPath = "RuntimeData/Presence/avatar_events.jsonl";
    bool bExternalAdaptersEnabled = false;
    std::string inboxPath = "RuntimeData/Presence/Inbox";
    std::string outboxPath = "RuntimeData/Presence/Outbox";
    int adapterPollMs = 150;
    int maxAdapterEventsPerMinute = 30;
    int maxAdapterTextCharacters = 4000;
    int maxAvatarEventBytes = 4194304;
    int rememberedAdapterIds = 2048;
    // Cap channel count; publicContextTurns separately bounds each channel history.
    int maxPublicConversationContexts = 32;
    // Cap consumed message envelopes by age and count.
    // Processed and Rejected archives are bounded separately.
    int adapterArchiveMaximumFiles = 200;
    int adapterArchiveMaximumAgeDays = 14;
    int publicContextTurns = 6;
    int streamReplyCooldownSeconds = 4;
    bool bRequireAddressedStreamMessages = true;
    bool bSpeakStreamReplies = false;
    std::vector<std::string> allowedAdapters = {"discord", "stream", "game"};
};
