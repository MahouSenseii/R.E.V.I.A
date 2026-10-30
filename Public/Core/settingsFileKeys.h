#pragma once

#include <array>
#include <string_view>

namespace revia::core
{

enum class OwnerKeyType
{
    Bool,
    Choice
};

// One Config/settings.json value the owner may change from the desktop's Settings dialog. The
// choice is kept in RuntimeData/Settings/owner-settings.json and laid over settings.json when it
// is loaded, because the build and every update rewrite settings.json from source.
struct OwnerKey
{
    std::string_view key; // the dotted JSON path, which is also the settings catalog key
    OwnerKeyType type;
    bool live;                // applied to the running session; otherwise it waits for a restart
    std::string_view choices; // Choice only: the allowed values, '|' separated
};

// The complete overlay allowlist. Anything not here is ignored when the overlay is read.
// In mcp.servers[*], the brackets name one server by its "id", the identifier configManager reads.
inline constexpr std::array kOwnerKeys = {
    OwnerKey{"vision.enabled", OwnerKeyType::Bool, true, ""},
    OwnerKey{"vision.continuousAwareness", OwnerKeyType::Bool, true, ""},
    OwnerKey{"perception.enabled", OwnerKeyType::Bool, false, ""},
    OwnerKey{"llm.visionEnabled", OwnerKeyType::Bool, false, ""},
    OwnerKey{"llm.imageMinTokens", OwnerKeyType::Choice, false, "0|256|512|1024|2048|4096"},
    OwnerKey{"llm.autoTune", OwnerKeyType::Bool, false, ""},
    OwnerKey{"llm.autoMaxTokens", OwnerKeyType::Bool, false, ""},
    OwnerKey{"llm.autoStartServer", OwnerKeyType::Bool, false, ""},
    OwnerKey{"llm.cloud.enabled", OwnerKeyType::Bool, true, ""},
    OwnerKey{"llm.cloud.useFor", OwnerKeyType::Choice, true, "never|ask|hardTurns"},
    OwnerKey{"embedding.enabled", OwnerKeyType::Bool, false, ""},
    OwnerKey{"embedding.autoStartServer", OwnerKeyType::Bool, false, ""},
    OwnerKey{"intelligence.enabled", OwnerKeyType::Bool, false, ""},
    OwnerKey{"intelligence.fast.enabled", OwnerKeyType::Bool, false, ""},
    OwnerKey{"intelligence.fast.warmAtStartup", OwnerKeyType::Bool, false, ""},
    OwnerKey{"intelligence.expert.enabled", OwnerKeyType::Bool, false, ""},
    OwnerKey{"intelligence.expert.onDemand", OwnerKeyType::Bool, false, ""},
    OwnerKey{"intelligence.expert.warmAtStartup", OwnerKeyType::Bool, false, ""},
    OwnerKey{"image.enabled", OwnerKeyType::Bool, false, ""},
    OwnerKey{"video.enabled", OwnerKeyType::Bool, false, ""},
    OwnerKey{"image.keepLoaded", OwnerKeyType::Bool, false, ""},
    OwnerKey{"image.offline", OwnerKeyType::Bool, false, ""},
    OwnerKey{"localApi.enabled", OwnerKeyType::Bool, false, ""},
    OwnerKey{"localApi.trustedPrivate", OwnerKeyType::Bool, false, ""},
    OwnerKey{"improvement.enabled", OwnerKeyType::Bool, false, ""},
    OwnerKey{"improvement.explore", OwnerKeyType::Bool, false, ""},
    OwnerKey{"improvement.verify", OwnerKeyType::Bool, false, ""},
    OwnerKey{"resources.autoPlan", OwnerKeyType::Bool, false, ""},
    OwnerKey{"resources.allowChatModelSplit", OwnerKeyType::Bool, false, ""},
    OwnerKey{"speechRecognition.useGpu", OwnerKeyType::Bool, false, ""},
    OwnerKey{"mcp.servers[*].enabled", OwnerKeyType::Bool, true, ""},
    OwnerKey{"mcp.servers[*].requireConfirmation", OwnerKeyType::Bool, true, ""},
};

}
