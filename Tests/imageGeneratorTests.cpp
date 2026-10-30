#include "Visual/imageGenerator.h"
#include "Audit/contentDigest.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <fstream>
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
        const unsigned char pixel[] = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 1, 0, 0, 0, 1, 8, 4, 0, 0, 0,
            181, 28, 12, 2, 0, 0, 0, 11, 73, 68, 65, 84, 120, 218, 99, 100, 248, 15, 0, 1, 5, 1, 1, 39, 24, 227, 102, 0, 0, 0, 0, 73, 69,
            78, 68, 174, 66, 96, 130};
        const auto bytes = std::make_shared<const std::string>(reinterpret_cast<const char*>(pixel), sizeof(pixel));
        revia::visual::ImageRequest reference;
        reference.mode = revia::visual::ImageRequestMode::ReferenceToImage;
        reference.prompt = "valid reference";
        reference.seed = 42;
        reference.source = revia::visual::ImageSource{"stored-pixels", revia::audit::ContentDigest(*bytes), "image/png", bytes, 1, 1};
        const auto edited = generator.Generate(reference);
        Check(edited.succeeded && edited.receipt.mode == reference.mode && edited.receipt.seed == 42 &&
                  edited.receipt.denoisingSteps == 2 && edited.receipt.sourceDigest == reference.source->digest,
            "Typed reference transport or receipt failed: " + edited.message);
        Check(std::filesystem::is_empty(root / "images" / ".inputs"), "Completed reference retained an input snapshot.");
        settings.steps = 1;
        settings.seed = 9;
        generator.Configure(settings);
        const auto raisedFloor = generator.Generate(reference);
        Check(raisedFloor.succeeded && raisedFloor.receipt.requestedSteps == 1 && raisedFloor.receipt.totalInferenceSteps == 2 &&
                  raisedFloor.receipt.denoisingSteps == 1 && raisedFloor.receipt.seed == 42,
            "Reference step-floor planning or explicit seed override was not preserved.");
        settings.steps = 4;
        generator.Configure(settings);
        const auto configuredSeed = generator.Generate("configured seed");
        Check(configuredSeed.succeeded && configuredSeed.receipt.seed == 9, "The legacy wrapper lost its configured seed.");
        for (const auto* prompt : {"wrong source", "wrong mode", "wrong seed", "zero denoise"})
        {
            reference.prompt = prompt;
            const auto invalidReceipt = generator.Generate(reference);
            Check(!invalidReceipt.succeeded && invalidReceipt.path.empty(), "A mismatched typed receipt was published.");
            Check(std::filesystem::is_empty(root / "images" / ".inputs"), "Failed receipt retained an input snapshot.");
        }
        reference.prompt = "wait";
        std::stop_source referenceStop;
        auto editing = std::async(std::launch::async, [&]() { return generator.Generate(reference, referenceStop.get_token()); });
        const auto referenceDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (generator.Snapshot().state != "loading" && std::chrono::steady_clock::now() < referenceDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Check(generator.Snapshot().state == "loading", "Reference cancellation never reached the provider.");
        referenceStop.request_stop();
        Check(editing.get().cancelled, "Reference generation ignored cancellation.");
        Check(std::filesystem::is_empty(root / "images" / ".inputs"), "Cancelled reference retained an input snapshot.");
        reference.prompt = "invalid strength";
        reference.strength = 0.2;
        const auto unsupported = generator.Generate(reference);
        Check(!unsupported.succeeded && !generator.Snapshot().loaded, "An unsupported zero-denoising strength reached a worker.");
        reference.strength = 0.5;
        reference.mode = revia::visual::ImageRequestMode::TextToImage;
        Check(!generator.Generate(reference).succeeded, "A text-to-image request silently discarded its supplied reference.");
        reference.mode = revia::visual::ImageRequestMode::ReferenceToImage;
        reference.source.reset();
        Check(!generator.Generate(reference).succeeded, "A reference request silently used text-to-image without a source.");
        Check(restarted.receipt.seed == restarted.expectedArtifact.seed, "A random seed was not resolved and verified by the host.");
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
