#include "Core/reviaApp.h"

#include "Core/crashDiagnostics.h"
#include "Core/exitReporter.h"
#include "Core/logger.h"
#include "Core/terminalText.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

void reviaApp::Run()
{
    // Before anything is printed: every string this program writes is UTF-8, and a
    // Windows console left on its OEM code page shows each non-ASCII character as two or
    // three wrong ones.
    const revia::core::Utf8ConsoleOutput utf8Output;

    // The terminal shell gets the same accounting as the desktop: a session that ends
    // without a recorded reason should be as visible here as it is there.
    revia::core::CrashDiagnostics::Install(ReviaLogDirectory());
    const std::string unrecordedPrevious =
        revia::core::ExitReporter::Begin(ReviaLogDirectory());
    if (!unrecordedPrevious.empty())
    {
        std::cout << "[Previous session] " << unrecordedPrevious << "\n\n";
    }

    session.SetConfirmationHandler([this](
        const revia::actions::ActionRequest& request,
        const revia::actions::PolicyDecision& decision)
    {
        return ConfirmAction(request, decision);
    });

    const auto subscription = session.Events().Subscribe([](
        const revia::runtime::RuntimeEvent& event)
    {
        if (event.kind == revia::runtime::RuntimeEventKind::Memory)
        {
            std::cout << "\n[Memory] " << event.message << "\n" << std::flush;
        }
        else if (event.kind == revia::runtime::RuntimeEventKind::AssistantMessage)
        {
            // Said outside a typed turn: a finished task, or her speaking up.
            std::cout << "\nRevia: " << event.message << "\n" << std::flush;
        }
        else if (event.kind == revia::runtime::RuntimeEventKind::Proposal)
        {
            std::cout << "\nRevia: " << event.message
                << "\n  Evidence: " << event.detail << "\n" << std::flush;
        }
        else if (event.kind == revia::runtime::RuntimeEventKind::SelfInquiry)
        {
            // Her own questions, shown before the answer they shaped. Never spoken.
            std::cout << "\n[Revia is thinking - " << event.detail << "]\n"
                << event.message << "\n" << std::flush;
        }
    });

    if (!session.Start())
    {
        revia::core::ExitReporter::Record(
            revia::core::ExitReason::StartupFailure,
            "the runtime session could not start");
        session.Events().Unsubscribe(subscription);
        session.Stop();
        return;
    }

    if (!session.Greeting().empty())
    {
        std::cout << session.DisplayName() << ": " << session.Greeting() << "\n\n";
    }

    // Memory, affect decay, and proposal events keep moving while the input reader waits.
    // The CLI is only a presentation surface; it must not become the clock for background
    // work.
    std::atomic<bool> polling = true;
    std::jthread pollWorker([this, &polling](const std::stop_token stopToken)
    {
        while (!stopToken.stop_requested() && polling.load())
        {
            session.PollBackgroundEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // Detached rather than joined: it spends its life inside a blocking read, which nothing
    // portable can interrupt. It owns a share of the state it writes to.
    std::thread([shared = input]()
    {
        revia::core::TerminalLineReader reader;
        std::string line;
        while (reader.ReadLine(line))
        {
            {
                std::lock_guard lock(shared->mutex);
                if (shared->confirmationsWaiting > 0 && !shared->confirmationLine.has_value())
                {
                    shared->confirmationLine = std::move(line);
                }
                else
                {
                    shared->chatLines.push_back(std::move(line));
                }
            }
            shared->changed.notify_all();
            line.clear();
        }
        {
            std::lock_guard lock(shared->mutex);
            shared->ended = true;
        }
        shared->changed.notify_all();
    }).detach();

    while (session.IsStarted())
    {
        std::cout << "You: " << std::flush;
        const std::optional<std::string> line = NextChatLine();
        if (!line.has_value())
        {
            break;
        }

        const revia::runtime::SessionResult result = session.Submit(*line);
        if (!result.text.empty())
        {
            std::cout << (result.fromAssistant ? session.DisplayName() : "System")
                << ": " << result.text << "\n\n";
        }
        if (!result.succeeded && !result.reason.empty())
        {
            std::cerr << "[Stopped] " << result.reason << '\n';
        }
        if (result.shouldExit)
        {
            revia::core::ExitReporter::Record(
                revia::core::ExitReason::UserCommand, "an exit command was typed");
            break;
        }
    }
    // Covers end-of-input as well: closing the terminal is still a reason worth naming.
    revia::core::ExitReporter::Record(
        revia::core::ExitReason::EventLoopEnded, "the input loop ended");

    polling.store(false);
    pollWorker.request_stop();
    pollWorker.join();
    session.Events().Unsubscribe(subscription);
    session.Stop();
}

std::optional<std::string> reviaApp::NextChatLine()
{
    std::unique_lock lock(input->mutex);
    input->changed.wait(lock, [this]
    {
        return !input->chatLines.empty() || input->ended;
    });
    if (input->chatLines.empty())
    {
        return std::nullopt;
    }
    std::string line = std::move(input->chatLines.front());
    input->chatLines.pop_front();
    return line;
}

revia::actions::ConfirmationChoice reviaApp::ConfirmAction(
    const revia::actions::ActionRequest& request,
    const revia::actions::PolicyDecision& decision)
{
    std::lock_guard asking(confirmationMutex);
    {
        // Registered before the question is printed, so an answer typed the instant it
        // appears is routed here rather than into the conversation.
        std::lock_guard lock(input->mutex);
        ++input->confirmationsWaiting;
    }
    std::cout << "\nAction: " << revia::actions::ToString(request.type) << '\n'
        << "Policy: " << revia::actions::ToString(decision.verdict)
        << " (" << decision.reason << ")\n"
        // "a" rather than a second yes/no question, so the standing answer stays a
        // deliberate keystroke and cannot be reached by holding down the ordinary one.
        << "Allow this action? [y]es / [a]ll of this task / [N]o: " << std::flush;
    std::string answer;
    {
        std::unique_lock lock(input->mutex);
        input->changed.wait(lock, [this]
        {
            return input->confirmationLine.has_value() || input->ended;
        });
        --input->confirmationsWaiting;
        if (!input->confirmationLine.has_value())
        {
            return revia::actions::ConfirmationChoice::Decline;
        }
        answer = std::move(*input->confirmationLine);
        input->confirmationLine.reset();
    }
    std::transform(answer.begin(), answer.end(), answer.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    if (answer == "a" || answer == "all")
    {
        return revia::actions::ConfirmationChoice::AllowForThisTask;
    }
    return answer == "y" || answer == "yes"
        ? revia::actions::ConfirmationChoice::Allow
        : revia::actions::ConfirmationChoice::Decline;
}
