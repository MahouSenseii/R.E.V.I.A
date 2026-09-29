#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace revia::runtime
{

// One completed interaction returned to a presentation shell.
//
// Kept separate from ReviaSession so conversation, vision, goals, and the CLI can share
// the result contract without depending on the lifecycle owner itself.
struct SessionResult
{
    bool succeeded = true;
    bool shouldExit = false;
    // A delta consumer received generated text. This reports delivery, not presentation;
    // the LLM layer never writes directly to a terminal or widget.
    bool wasStreamed = false;
    // Set only when this reply was published sentence by sentence as it was spoken, so
    // the shell has already displayed all of it and must not append it a second time.
    bool spokenAsFragments = false;
    // What Revia did to produce this reply: her posture, any reasoning the model emitted,
    // and where the time went. Shown collapsed in the shell so it is available without
    // being in the way.
    std::string reasoning;
    bool fromAssistant = false;
    std::string text;
    std::string reason;
    // Set when this reply was handed to the speech worker. The shell holds the text until
    // the matching Speaking event so the words appear with the voice rather than well
    // ahead of it. A reply that will not be spoken leaves this false and is shown at once.
    bool speechPending = false;
    std::uint64_t utteranceId = 0;
    // The web sources this reply drew on, one line each ("[1] Title — https://..."),
    // for the shell to show under the text. Only addresses the lookup actually
    // returned: a citation the model wrote that names no such source is not here.
    // Never spoken; a read-out URL is noise, and the line on screen is the point.
    std::vector<std::string> sources;
};

// The sources as the shells show them, under the reply: a blank line, "Sources:", one
// line each. Empty when there are none, so a shell can append it unconditionally.
[[nodiscard]] inline std::string RenderSourcesFooter(const std::vector<std::string>& sources)
{
    if (sources.empty()) return {};
    std::string footer = "\n\nSources:";
    for (const std::string& source : sources)
    {
        footer += "\n" + source;
    }
    return footer;
}

} // namespace revia::runtime
