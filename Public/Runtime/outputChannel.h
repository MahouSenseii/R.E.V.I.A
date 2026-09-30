#pragma once

// Where a reply is going. Revia speaks when she is talking to the person in front of her;
// text she is composing into someone else's application is not something to read aloud.
enum class outputChannel
{
    LocalVoice,
    ExternalApplication
};
