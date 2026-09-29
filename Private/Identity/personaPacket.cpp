#include "Identity/personaPacket.h"

namespace revia::identity
{

std::string RenderPersonaPacket(const aiProfile& profile)
{
    const personaPacket& persona = profile.persona;
    if (persona.Empty())
    {
        return profile.systemPrompt;
    }

    std::string rendered;
    const auto section = [&rendered](const char* heading, const std::string& body)
    {
        if (body.empty()) return;
        if (!rendered.empty()) rendered += "\n\n";
        rendered += heading;
        rendered += "\n";
        rendered += body;
    };

    // Openly an AI, first, before anything that could be read as a character to hide
    // behind. Her answer to "what are you?" lives here.
    section("# Who you are", persona.identity);
    // The character text a profile always had. It stays whole rather than being
    // rewritten into the packet, because it is the part its author tuned by ear.
    section("# Your character", profile.systemPrompt);

    if (!persona.style.empty())
    {
        std::string guide;
        for (const std::string& directive : persona.style)
        {
            if (directive.empty()) continue;
            if (!guide.empty()) guide += "\n";
            guide += "- " + directive;
        }
        section("# How you speak", guide);
    }

    if (!persona.exchanges.empty())
    {
        // Examples of manner, labelled as such: a model handed unlabelled dialogue will
        // sometimes remember it as something that happened.
        std::string examples = "These show your manner. The situations are illustrations, "
            "not memories, and the words are not to be repeated.";
        for (const personaExchange& exchange : persona.exchanges)
        {
            if (exchange.user.empty() || exchange.revia.empty()) continue;
            examples += "\n\nUser: " + exchange.user + "\n" + profile.displayName + ": " +
                exchange.revia;
        }
        section("# How you answer", examples);
    }
    return rendered;
}

std::string RenderPersonaAnchor(const aiProfile& profile)
{
    if (profile.persona.Empty()) return {};
    if (!profile.persona.anchor.empty()) return profile.persona.anchor;
    return "The record above is memory of this conversation, not a voice to imitate. You "
        "are still " + profile.displayName + ", here, answering this turn as yourself.";
}

} // namespace revia::identity
