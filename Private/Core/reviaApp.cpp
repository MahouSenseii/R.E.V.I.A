#include "Core/reviaApp.h"

#include "Core/crashDiagnostics.h"
#include "Core/exitReporter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <iostream>
#include <string>
#include <thread>

void reviaApp::Run()
{
    // The terminal shell gets the same accounting as the desktop: a session that ends
    // without a recorded reason should be as visible here as it is there.
    revia::core::CrashDiagnostics::Install("logs");
    const std::string unrecordedPrevious = revia::core::ExitReporter::Begin("logs");
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

    // Memory, affect decay, and proposal events keep moving while getline waits. The CLI
    // is only a presentation surface; it must not become the clock for background work.
    std::atomic<bool> polling = true;
    std::jthread pollWorker([this, &polling](const std::stop_token stopToken)
    {
        while (!stopToken.stop_requested() && polling.load())
        {
            session.PollBackgroundEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    std::string input;
    while (session.IsStarted())
    {
        std::cout << "You: " << std::flush;
        if (!std::getline(std::cin, input))
        {
            break;
        }

        const revia::runtime::SessionResult result = session.Submit(input);
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

bool reviaApp::ConfirmAction(
    const revia::actions::ActionRequest& request,
    const revia::actions::PolicyDecision& decision) const
{
    std::cout << "Action: " << revia::actions::ToString(request.type) << '\n'
        << "Policy: " << revia::actions::ToString(decision.verdict)
        << " (" << decision.reason << ")\n"
        << "Allow this action? [y/N]: " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer))
    {
        return false;
    }
    std::transform(answer.begin(), answer.end(), answer.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return answer == "y" || answer == "yes";
}
