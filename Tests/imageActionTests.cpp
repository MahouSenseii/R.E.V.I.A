#include "Actions/actionRuntime.h"
#include "Audit/contentDigest.h"
#include "Policy/capabilityProjection.h"
#include "Runtime/documentWorkshop.h"
#include "Visual/imageGenerator.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
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
        return 2;
    using namespace revia;
    const auto root = std::filesystem::temp_directory_path() / ("revia-image-action-" + actions::NewActionId());
    std::filesystem::create_directories(root);
    int status = 0;
    try
    {
        const auto config = root / "capabilities.json";
        std::ofstream(config) << R"({"mode":"approved_scope","approvedRoots":[],"createMissingApprovedRoots":false})";
        actions::ActionRuntime runtime;
        std::string error;
        const auto journal = root / "audit.jsonl";
        Check(runtime.Initialize(config, journal, error), error);
        const auto parsed = runtime.ParseJson(R"({"action":"generate_image","prompt":"valid"})");
        Check(parsed.succeeded, "The admitted image action was not parsed: " + parsed.error);
        Check(!runtime.ParseJson(R"({"action":"generate_image","prompt":"valid","outputPath":"C:/foreign.png"})").succeeded,
            "Model output could choose an image destination.");
        Check(!runtime.Execute(parsed.request).Succeeded(), "An unbound image provider executed.");
        imageSettings settings;
        settings.bEnabled = true;
        settings.pythonExecutable = argv[1];
        settings.serviceScript = argv[2];
        settings.outputPath = (root / "images").string();
        settings.logDirectory = (root / "logs").string();
        settings.width = settings.height = 256;
        settings.port = 18095;
        settings.startupTimeoutSeconds = 5;
        visual::ImageGenerator generator;
        generator.Configure(settings);
        runtime.BindImageProvider(generator, settings.outputPath, false);
        auto authority = std::make_shared<policy::CompanionAuthority>();
        runtime::RuntimeStamp stamp;
        stamp.companionId = "image-companion";
        stamp.sessionId = "image-session";
        stamp.generation = 1;
        policy::AuthorityPermissions imageOnly;
        imageOnly.operations = {actions::ActionType::GenerateImage};
        authority->SetCompanionDefaults(stamp.companionId, imageOnly);
        Check(authority->RegisterSession(stamp), "Image test session was not admitted.");
        runtime.BindAuthority(authority, stamp);
        auto request = parsed.request;
        request.dryRun = true;
        const auto preview = runtime.ExecuteFor(stamp, request);
        Check(preview.Succeeded() && !preview.result.attempted && !preview.result.image, "An image dry run started a provider effect.");
        request.dryRun = false;
        const auto generated = runtime.ExecuteFor(stamp, request);
        Check(generated.Succeeded() && generated.result.image && generated.result.image->succeeded,
            "Admitted generation did not produce a verified artifact: " + generated.Message());
        Check(std::filesystem::exists(generated.result.image->path), "Image receipt referenced no file.");
        messageRouter router((root / "memory.db").string());
        visual::DiagramStore diagrams(root / "diagrams");
        logger log(root / "logs");
        runtime::DocumentWorkshop workshop(router, runtime, diagrams, log);
        const auto hasCanvas = [](const runtime::TurnOutcome& turn)
        {
            return std::any_of(turn.events.begin(), turn.events.end(), [](const auto& event)
                { return event.kind == runtime::TurnEvent::Kind::Runtime && event.runtimeEvent.component == "Canvas"; });
        };
        const auto refused = workshop.GenerateImage("valid", {}, [] { return false; }, stamp);
        Check(!refused.result.succeeded && !hasCanvas(refused), "Expired image admission reported success or published Canvas.");
        const auto foreground = workshop.GenerateImage("valid", {}, {}, stamp);
        Check(foreground.result.succeeded && hasCanvas(foreground), "DocumentWorkshop bypassed or failed admitted image publication.");
        const auto background = workshop.GenerateImage("valid", {}, {}, stamp, false);
        Check(background.result.succeeded && !hasCanvas(background), "Background generation published a Canvas event.");
        actions::ActionRuntime noAudit;
        Check(noAudit.Initialize(config, root, error), error);
        noAudit.BindImageProvider(generator, settings.outputPath, false);
        const auto refusedAudit = noAudit.Execute(parsed.request);
        Check(!refusedAudit.Succeeded() && !refusedAudit.result.attempted && !refusedAudit.result.image,
            "Image generation began without a writable durable audit journal.");
        const auto captured = runtime.Settings();
        auto reduced = captured;
        reduced.image.enabled = false;
        Check(!policy::ProjectCapabilityScope(captured, reduced).image.enabled, "Tool projection retained revoked image authority.");
        reduced = captured;
        reduced.image.outputRoot = root / "foreign";
        Check(!policy::ProjectCapabilityScope(captured, reduced).image.enabled, "Tool projection changed the fixed image destination.");
        Check(!runtime.ExecuteScopedFor(stamp, request, policy::CapabilityPolicy(reduced)).Succeeded(),
            "A scope could replace the host-owned image destination.");
        request.requestedBy = "autonomous:create";
        Check(!runtime.ExecuteFor(stamp, request).Succeeded(), "Autonomous art inherited foreground permission.");
        runtime.BindImageProvider(generator, settings.outputPath, true);
        Check(runtime.ExecuteFor(stamp, request).Succeeded(), "Explicitly admitted autonomous art failed.");
        Check(runtime.SetExecutionMode(actions::ExecutionMode::Disabled, error), error);
        Check(!runtime.ExecuteFor(stamp, request).Succeeded(), "Disabled execution mode allowed image effects.");
        Check(runtime.SetExecutionMode(actions::ExecutionMode::ApprovedScope, error), error);
        request.requestedBy = "user";
        request.value = "wait";
        std::atomic_bool admitted = true;
        request.beforeEffect = [&](const std::string&) { return admitted.load() ? std::string{} : "Audience changed."; };
        auto work = std::async(std::launch::async, [&] { return runtime.ExecuteFor(stamp, request); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (generator.Snapshot().state != "loading" && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        admitted.store(false);
        const auto cancelled = work.get();
        Check(!cancelled.Succeeded() && cancelled.result.image && cancelled.result.image->cancelled,
            "Changing audience did not interrupt an admitted image job.");
        authority->EndSession(stamp);
        Check(!runtime.ExecuteFor(stamp, parsed.request).Succeeded(), "A retired session generated another image.");
        std::ifstream audit(journal);
        std::string line;
        bool receiptSeen = false;
        while (std::getline(audit, line))
        {
            const auto record = nlohmann::json::parse(line);
            if (record.value("record_type", "") == "result" && record.value("succeeded", false) && !record.value("dry_run", false))
            {
                Check(record.at("action") == "generate_image", "Image completion was audited as another action.");
                const auto& receipt = record.at("image").at("receipt");
                Check(!receipt.at("sha256").get<std::string>().empty() && receipt.at("width") == 256,
                    "The image audit omitted verified native provenance.");
                receiptSeen = true;
            }
        }
        Check(receiptSeen, "No successful image action receipt was audited.");
        generator.Shutdown();
        std::cout << "Admitted image execution, private output, audit, autonomy and stale-scope tests passed.\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    return status;
}
