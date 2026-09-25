#pragma once

#include "Runtime/reviaSession.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

// Thin terminal shell. ReviaSession is the only runtime owner used by both desktop and
// CLI, so behavior, workers, cancellation, memory, and shutdown cannot drift between two
// separate application implementations.
class reviaApp
{
public:
    void Run();

private:
    // Every line of standard input, read by one thread and handed to whoever is waiting.
    // Goals ask for approval from their own worker while the chat loop is blocked on the
    // next line; two threads reading std::cin at once gave the answer to whichever won,
    // so a typed "y" could become a chat turn and the next sentence a refusal.
    struct TerminalInput
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<std::string> chatLines;
        std::optional<std::string> confirmationLine;
        int confirmationsWaiting = 0;
        bool ended = false;
        // The chat loop has ended: nobody will answer another question.
        bool closing = false;
    };

    std::optional<std::string> NextChatLine();
    revia::actions::ConfirmationChoice ConfirmAction(
        const revia::actions::ActionRequest& request,
        const revia::actions::PolicyDecision& decision);

    // Shared with a detached reader: a blocking terminal read cannot be interrupted, so the
    // reader may still be blocked on it after Run returns and must not outlive what it
    // writes to.
    std::shared_ptr<TerminalInput> input = std::make_shared<TerminalInput>();
    // One approval question on screen at a time, however many workers ask.
    std::mutex confirmationMutex;
    revia::runtime::ReviaSession session;
};
