#include "Visual/imageGenerator.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
void Check(const bool condition, const std::string& reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}
}

int main(const int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: imageGeneratorTests <python.exe> <fixture.py>\n";
        return 2;
    }
    const auto root = std::filesystem::temp_directory_path() /
                      ("revia-image-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try
    {
        imageSettings settings;
        settings.bEnabled = true;
        settings.pythonExecutable = argv[1];
        settings.serviceScript = argv[2];
        settings.outputPath = (root / "images").string();
        settings.logDirectory = (root / "logs").string();
        settings.width = settings.height = 256;
        settings.port = 18093;
        settings.startupTimeoutSeconds = 5;
        settings.requestTimeoutSeconds = 10;
        revia::visual::ImageGenerator generator;
        generator.Configure(settings);
        std::stop_source cancel;
        auto work = std::async(std::launch::async, [&]() { return generator.Generate("wait", {}, cancel.get_token()); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (generator.Snapshot().state != "loading" && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        Check(generator.Snapshot().state == "loading", "Native generation never reached loading.");
        const auto stopped = std::chrono::steady_clock::now();
        cancel.request_stop();
        const auto cancelled = work.get();
        Check(cancelled.cancelled && !cancelled.succeeded, "Stop did not cancel native image generation.");
        Check(std::chrono::steady_clock::now() - stopped < std::chrono::seconds(4), "Stop waited for the generation timeout.");
        Check(!generator.Snapshot().loaded, "Cancelled worker retained image weights.");

        const auto restarted = generator.Generate("valid");
        Check(restarted.succeeded && std::filesystem::exists(restarted.path), "Restart did not produce verified PNG: " + restarted.message);
        Check(!generator.Snapshot().loaded, "Default image lifetime retained weights after completion.");
        const auto refused = generator.Generate("corrupt");
        Check(!refused.succeeded, "Native publication accepted corrupt image bytes.");
        const auto malformed = generator.Generate("wrong metadata");
        Check(!malformed.succeeded && malformed.path.empty(), "Malformed completion still reported a successful artifact.");
        const auto stale = generator.Generate("valid", {}, {}, []() { return false; });
        Check(stale.cancelled && !stale.succeeded, "Retired admission started another image job.");
        generator.Shutdown();
        std::filesystem::remove_all(root);
        std::cout << "Native image cancellation, restart, release, decode and admission tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
