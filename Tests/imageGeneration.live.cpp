#include "Visual/imageGenerator.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

int main(const int argc, char** argv)
{
    if (argc != 5 && argc != 7)
    {
        std::cerr << "Usage: imageGeneration.live <python> <service> <cache> <output> [model variant]\n";
        return 2;
    }
    imageSettings settings;
    settings.bEnabled = true;
    settings.pythonExecutable = argv[1];
    settings.serviceScript = argv[2];
    settings.cacheDirectory = argv[3];
    settings.outputPath = argv[4];
    settings.logDirectory = (std::filesystem::path(argv[4]) / "logs").string();
    settings.port = 18094;
    if (argc == 7)
    {
        settings.model = argv[5];
        settings.variant = argv[6];
        settings.minimumFreeVramMiB = 7000;
    }
    settings.seed = 42;
    settings.requestTimeoutSeconds = 180;
    revia::visual::ImageGenerator generator;
    generator.Configure(settings);
    nlohmann::json report;
    report["model"] = settings.model;
    report["seed"] = settings.seed;
    const std::string prompt = "A hand-painted watercolor illustration of a red fox with a blue scarf sitting under a snow-covered pine "
                               "tree, soft morning light, full-body animal portrait, textured watercolor paper";
    const auto first = generator.Generate(prompt);
    report["generation"] = {{"succeeded", first.succeeded}, {"message", first.message}, {"detail", first.detail},
        {"path", first.path.string()}, {"elapsedMs", first.elapsedMilliseconds}, {"loadMs", first.modelLoadMilliseconds},
        {"inferenceMs", first.inferenceMilliseconds}, {"peakAllocatedMiB", first.peakAllocatedMiB},
        {"peakReservedMiB", first.peakReservedMiB}, {"weightsReleased", !generator.Snapshot().loaded}, {"prompt", prompt}};
    if (first.succeeded)
    {
        std::stop_source cancellation;
        auto task = std::async(std::launch::async, [&]() { return generator.Generate(prompt, {}, cancellation.get_token()); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (generator.Snapshot().state != "generating" && std::chrono::steady_clock::now() < deadline &&
               task.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto before = generator.Snapshot();
        const auto cancelledAt = std::chrono::steady_clock::now();
        cancellation.request_stop();
        const auto stopped = task.get();
        report["cancellation"] = {{"cancelled", stopped.cancelled}, {"succeeded", stopped.succeeded}, {"stateAtStop", before.state},
            {"stepAtStop", before.step}, {"message", stopped.message},
            {"stopLatencyMs", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cancelledAt).count()},
            {"weightsReleased", !generator.Snapshot().loaded}};
        const auto repeat = generator.Generate(prompt);
        report["restart"] = {{"succeeded", repeat.succeeded}, {"detail", repeat.detail}, {"path", repeat.path.string()},
            {"elapsedMs", repeat.elapsedMilliseconds}, {"loadMs", repeat.modelLoadMilliseconds},
            {"inferenceMs", repeat.inferenceMilliseconds}, {"peakAllocatedMiB", repeat.peakAllocatedMiB},
            {"peakReservedMiB", repeat.peakReservedMiB}};
    }
    generator.Shutdown();
    std::filesystem::create_directories(settings.outputPath);
    std::ofstream(std::filesystem::path(settings.outputPath) / "acceptance.json") << report.dump(2);
    std::cout << report.dump(2) << '\n';
    return first.succeeded && report.contains("restart") && report["restart"]["succeeded"].get<bool>() &&
                   report["cancellation"]["cancelled"].get<bool>()
               ? 0
               : 1;
}
