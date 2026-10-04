#pragma once

#include "Runtime/runtimeStamp.h"

#include <cstdint>
#include <string>

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
    // Reports submission to the speech worker. Shells can display approved text immediately.
    bool speechPending = false;
    std::uint64_t utteranceId = 0;
    RuntimeStamp stamp;
    std::uint64_t audienceRevision = 0;
};

} // namespace revia::runtime
