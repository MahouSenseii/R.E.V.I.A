#include "Runtime/reviaSession.h"

#include "Core/localApiToken.h"

namespace revia::runtime
{

void ReviaSession::StartLocalApi()
{
    if (!settings.localApi.bEnabled)
        return;
    std::string error;
    // One token per install, not per companion: it identifies this PC's owner to the
    // programs they gave it to.
    const std::string token = core::LoadOrCreateLocalApiToken(companionPaths.InstallRoot() / "RuntimeData/LocalApi/api-token", error);
    if (token.empty())
    {
        PublishComponent("Local API", "Disabled", error);
        return;
    }
    auto api = std::make_shared<presence::LocalApiRuntime>(settings.llm, [this] { return !started.load() || busy.load(); });
    presence::LocalApiRuntime::PrivateTurn privateTurn;
    if (settings.localApi.bTrustedPrivate)
    {
        // The owner chose this: each request is typed into her own conversation, with
        // memory, tools and approvals exactly as at the keyboard.
        privateTurn = [this](const std::string& input, std::stop_token) { return Submit(input); };
    }
    const std::string address = "http://127.0.0.1:" + std::to_string(settings.localApi.port) + "/v1";
    if (!api->Start(settings.localApi.port, token, std::move(privateTurn)))
    {
        PublishComponent("Local API", "Disabled", "The local API could not listen on " + address + ".");
        return;
    }
    localApiToken = token;
    localApiRuntime.store(std::move(api));
    if (settings.localApi.bTrustedPrivate)
    {
        appLogger.Warning("The local API is TRUSTED: any program with its token speaks to Revia as the owner.");
        PublishComponent("Local API", "Enabled", "Listening on " + address + ". Trusted: each request is an owner turn.");
    }
    else
    {
        PublishComponent("Local API", "Enabled", "Listening on " + address + " as a shared audience.");
    }
}

std::string ReviaSession::LocalApiStatusText() const
{
    const auto api = localApiRuntime.load();
    if (!api)
    {
        return settings.localApi.bEnabled
                   ? "The local API is enabled in settings but is not running; see the Local API status in the log for why."
                   : "The local API is OFF. To let other programs on this PC talk to Revia, set \"localApi\": {\"enabled\": true} in "
                     "Config/settings.json and restart her.";
    }
    const std::string address = "http://127.0.0.1:" + std::to_string(api->Port()) + "/v1";
    return "Local API: " + address + " (OpenAI-compatible: GET /models, POST /chat/completions)\n" +
           (api->Trusted() ? "Mode: TRUSTED - each request is your own turn, with your memory, tools and approvals.\n"
                           : "Mode: shared audience - no private memory, screen, clipboard or tools.\n") +
           "Token: " + localApiToken +
           "\nKeep the token private: any program on this PC that has it can talk to Revia. Delete "
           "RuntimeData/LocalApi/api-token and restart her to replace it.\n"
           "Example: curl " + address + "/models -H \"Authorization: Bearer <token>\"";
}

} // namespace revia::runtime
