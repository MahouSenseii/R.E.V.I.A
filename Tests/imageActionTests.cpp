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
#include <memory>
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
        for (const auto* field : {"source", "sourceId", "sourceDigest", "mode", "seed", "receipt"})
        {
            nlohmann::json untrusted{{"action", "generate_image"}, {"prompt", "valid"}, {field, "untrusted"}};
            Check(!runtime.ParseJson(untrusted.dump()).succeeded, "Model JSON supplied host generation provenance.");
        }
        Check(!runtime.Execute(parsed.request).Succeeded(), "An unbound image provider executed.");
        Check(!runtime.ParseJson(R"({"action":"generate_video","prompt":"a paper boat","source":"C:/foreign.png"})").succeeded,
            "Model action JSON supplied video source authority.");
        actions::CapabilitySettings videoCapabilities;
        videoCapabilities.mode = actions::ExecutionMode::ApprovedScope;
        videoCapabilities.video = {true, false, root / "videos"};
        actions::ActionRequest nativeVideo;
        nativeVideo.type = actions::ActionType::GenerateVideo;
        nativeVideo.value = "a paper boat";
        nativeVideo.videoRequest = visual::VideoRequest{};
        nativeVideo.videoRequest->prompt = nativeVideo.value;
        Check(policy::CapabilityPolicy(videoCapabilities).Evaluate(nativeVideo).verdict == actions::PolicyVerdict::Allowed,
            "A bounded host-owned video request was not admitted.");
        nativeVideo.source = root / "foreign.png";
        Check(policy::CapabilityPolicy(videoCapabilities).Evaluate(nativeVideo).verdict == actions::PolicyVerdict::Blocked,
            "A video action accepted a caller-selected source path.");
        nativeVideo.source.clear();
        auto changedVideoCapabilities = videoCapabilities;
        changedVideoCapabilities.video.outputRoot = root / "other-videos";
        Check(!policy::ProjectCapabilityScope(videoCapabilities, changedVideoCapabilities).video.enabled,
            "A captured video scope silently changed its host-owned output root.");
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
        std::ifstream sourceFile(generated.result.image->path, std::ios::binary);
        const std::string sourceBytes((std::istreambuf_iterator<char>(sourceFile)), std::istreambuf_iterator<char>());
        sourceFile.close();
        visual::ImageRequest referenceRequest;
        referenceRequest.mode = visual::ImageRequestMode::ReferenceToImage;
        referenceRequest.prompt = "valid";
        referenceRequest.seed = 42;
        referenceRequest.source = visual::ImageSource{
            "source-fixture", audit::ContentDigest(sourceBytes), "image/png", std::make_shared<const std::string>(sourceBytes), 256, 256};
        request.imageRequest = referenceRequest;
        const auto edited = runtime.ExecuteFor(stamp, request);
        Check(edited.Succeeded() && edited.result.image &&
                  edited.result.image->receipt.mode == visual::ImageRequestMode::ReferenceToImage &&
                  edited.result.image->receipt.sourceId == referenceRequest.source->id &&
                  edited.result.image->receipt.sourceDigest == referenceRequest.source->digest && edited.result.image->receipt.seed == 42 &&
                  edited.result.image->receipt.denoisingSteps > 0,
            "The action did not preserve its typed reference through actual worker receipt verification.");
        request.imageRequest->prompt = "another prompt";
        Check(!runtime.ExecuteFor(stamp, request).Succeeded(), "The typed image prompt disagreed with the audited action prompt.");
        request.imageRequest.reset();
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
        Check(foreground.result.succeeded && hasCanvas(foreground) && foreground.generatedImage && foreground.generatedImage->succeeded,
            "DocumentWorkshop lost the verified native artifact while publishing its image.");
        const auto background = workshop.GenerateImage("valid", {}, {}, stamp, false);
        Check(background.result.succeeded && !hasCanvas(background) && background.generatedImage && background.generatedImage->succeeded,
            "Background generation published Canvas or lost its typed artifact.");
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
        const auto contractFor = [](runtime::RuntimeStamp origin)
        {
            core::TaskContract contract;
            if (origin.taskId.empty())
                origin.taskId = "image-action-fixture";
            if (origin.attemptId.empty())
                origin.attemptId = "image-action-attempt";
            contract.stamp = origin;
            contract.scope.companionId = origin.companionId;
            contract.scope.audience = {identity::AudienceKind::Private, "image-private", 1, {}};
            contract.goal = "Produce the admitted fixture output";
            contract.deliverables = {"A bounded native result"};
            contract.acceptanceObligations = {"Keep execution and native verification independently admitted"};
            contract.cancellation.origin = origin;
            contract.sourceKind = "chat";
            contract.sourceId = "fixture-calculation";
            return contract;
        };
        runtime.BindTaskContracts(
            [&](const auto& action, const auto& origin, auto)
            {
                auto contract = contractFor(origin);
                contract.sourceKind = "action";
                contract.sourceId = action.id;
                return std::make_shared<const core::TaskContract>(std::move(contract));
            },
            [stamp](const auto& contract, auto token)
            {
                return !token.stop_requested() && contract.stamp.SameSession(stamp) && core::ValidateTaskContract(contract)
                           ? std::string{}
                           : std::string("The fixture contract retired.");
            });
        auto verificationContract = contractFor(stamp);
        verificationContract.stamp.policyVersion = authority->Revision();
        verificationContract.cancellation.origin = verificationContract.stamp;
        request.requestedBy = "user";
        request.value = "wait";
        std::atomic_bool admitted = true;
        request.beforeEffect = [&](const std::string&) { return admitted.load() ? std::string{} : "Audience changed."; };
        auto work = std::async(std::launch::async, [&] { return runtime.ExecuteFor(stamp, request); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (generator.Snapshot().state != "loading" && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto readiness = std::async(std::launch::async, [&] { return runtime.IsInitialized(); });
        const bool readinessDidNotWait = readiness.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready;
        auto journalRead = std::async(std::launch::async, [&] { return runtime.EvidenceJournalOwner(); });
        const bool journalDidNotWait = journalRead.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready;
        auto verification = std::async(std::launch::async,
            [&]
            {
                return runtime.RecordLocalVerificationFor(
                    stamp, verificationContract, audit::ContentDigest("declared native calculation"), {}, [] { return true; });
            });
        const bool verificationDidNotWait = verification.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready;
        admitted.store(false);
        const auto cancelled = work.get();
        Check(readiness.get() && readinessDidNotWait, "Conversation readiness waited behind a media action's execution mutex.");
        Check(journalRead.get() && journalDidNotWait, "Conversation journal access waited behind a media action's execution mutex.");
        const auto verified = verification.get();
        Check(verified && verificationDidNotWait && verified->scope.audience.audienceId == "image-private" &&
                  verified->stamp.taskId == verificationContract.stamp.taskId,
            "Admitted native verification waited behind a media action or lost its scoped durable receipt.");
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
