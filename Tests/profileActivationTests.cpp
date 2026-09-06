#include "reviaSessionTestAccess.h"
#include "Core/preferenceStore.h"
#include "Speech/voicePresetStore.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <future>
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>

namespace
{
using namespace std::chrono_literals;
using namespace revia::runtime;
using revia::identity::Trait;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using Access = ReviaSessionTestAccess;
using json = nlohmann::json;

class WorkingDirectory
{
public:
    explicit WorkingDirectory(const std::filesystem::path& root)
        : previous(std::filesystem::current_path()) { std::filesystem::current_path(root); }
    ~WorkingDirectory() { std::filesystem::current_path(previous); }
private:
    std::filesystem::path previous;
};

void Write(const std::filesystem::path& path, const json& value)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << value.dump(2);
    output.close();
    Check(!output.fail(), "Could not write a profile fixture.");
}

template<class Predicate>
bool Until(Predicate predicate)
{
    const auto end = std::chrono::steady_clock::now() + 5s;
    do
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

class Backend
{
public:
    Backend()
    {
        server.Get("/health", [](const auto&, auto& response)
        { response.set_content(R"({"status":"ok","slots_idle":1})", "application/json"); });
        server.Get("/v1/models", [](const auto&, auto& response)
        { response.set_content(R"({"data":[{"id":"fixture-main"}]})", "application/json"); });
        server.Get("/props", [](const auto&, auto& response)
        { response.set_content(R"({"total_slots":1,"default_generation_settings":{"n_ctx":8192}})", "application/json"); });
        server.Post("/v1/chat/completions", [this](const auto& request, auto& response)
        {
            const auto body = json::parse(request.body);
            {
                std::lock_guard lock(mutex);
                requests.push_back(body);
            }
            if (body.dump().find("blocked-profile-turn") != std::string::npos && block.load())
            {
                entered = true;
                Until([&] { return !block.load(); });
            }
            const json chunk = {{"choices", json::array({{
                {"delta", {{"content", "Maple leaves have pointed lobes. Oak leaf shapes vary by species, with rounded or pointed lobes."}}},
                {"finish_reason", "stop"}}})}};
            response.set_content("data: " + chunk.dump() + "\n\ndata: [DONE]\n\n", "text/event-stream");
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind the profile backend fixture.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        if (!Until([&] { return server.is_running(); }))
        {
            server.stop();
            thread.join();
            Check(false, "Profile backend fixture did not start.");
        }
    }
    ~Backend() { block = false; server.stop(); thread.join(); }

    json RequestFor(const std::string& marker)
    {
        std::lock_guard lock(mutex);
        for (auto it = requests.rbegin(); it != requests.rend(); ++it)
        {
            if (!it->value("stream", false)) continue;
            const auto& messages = it->at("messages");
            for (auto message = messages.rbegin(); message != messages.rend(); ++message)
            {
                if (message->value("role", "") != "user") continue;
                if (message->at("content").dump().find(marker) != std::string::npos) return *it;
                break;
            }
        }
        Check(false, "A real conversation request never reached the fixture: " + marker);
        return {};
    }

    int port = 0;
    std::atomic<bool> block{false}, entered{false};
private:
    httplib::Server server;
    std::mutex mutex;
    std::vector<json> requests;
    std::jthread thread;
};

void Configure(const std::filesystem::path& root, int port)
{
    Write(root / "Config/settings.json", {
        {"activeProfile", "alpha"},
        {"llm", {{"backend", "LLamaCpp"}, {"host", "127.0.0.1"}, {"port", port},
            {"modelName", "fixture-main"}, {"autoStartServer", false}, {"visionEnabled", false},
            {"temperature", 0.45}, {"maxTokens", 321}, {"autoMaxTokens", false},
            {"modelPath", (root / "absent.gguf").string()},
            {"mediaPath", (root / "RuntimeData/Vision").string()}}},
        {"intelligence", {{"enabled", false}}},
        {"embedding", {{"enabled", false}, {"autoStartServer", false}}},
        {"speech", {{"enabled", false}, {"backend", "WindowsSapi"}, {"speakGreeting", false},
            {"voiceDataPath", (root / "RuntimeData/Voices").string()}}},
        {"speechRecognition", {{"enabled", false}}},
        {"presence", {{"enabled", false}, {"externalAdaptersEnabled", false}}},
        {"vision", {{"enabled", false}}}, {"perception", {{"enabled", false}}},
        {"initiative", {{"enabled", false}}}, {"bargeIn", {{"enabled", false}}},
        {"conversation", {{"archiveEnabled", false}}}, {"image", {{"enabled", false}}},
        {"resources", {{"usageSampleSeconds", 0}}},
        {"responseFilter", {{"aiReviewEnabled", false}}}
    });
    Write(root / "Config/Profiles/alpha.json", {
        {"id", "authored-alpha"}, {"displayName", "Alpha"}, {"systemPrompt", "Profile alpha marker."},
        {"shouldSpeak", false}, {"memoryEnabled", false}, {"temperature", 0.2}, {"maxTokens", 137},
        {"answerObligation", "reliable"}, {"personalityBaseline", {{"patience", 0.2}}},
        {"preferences", json::array({{{"subject", "fixture astronomy"}, {"strength", -0.5}}})}
    });
    Write(root / "Config/Profiles/beta.json", {
        {"id", "authored-beta"}, {"displayName", "Beta"}, {"systemPrompt", "Profile beta marker."},
        {"shouldSpeak", false}, {"memoryEnabled", false}, {"temperature", 0.8}, {"maxTokens", 211},
        {"answerObligation", "characterFirst"}, {"personalityBaseline", {{"patience", 0.8}}},
        {"preferences", json::array({{{"subject", "fixture astronomy"}, {"strength", 0.9}},
            {{"subject", "fixture botany"}, {"strength", 0.6}}})}
    });
    const auto voiceRoot = root / "RuntimeData/Voices";
    std::filesystem::create_directories(voiceRoot);
    const auto reference = voiceRoot / "reference.wav";
    const unsigned char wave[] = {
        'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,1,0,0x80,0x3e,0,0,0,0x7d,0,0,2,0,16,0,'d','a','t','a',4,0,0,0,0,0,0,0};
    std::ofstream audio(reference, std::ios::binary);
    audio.write(reinterpret_cast<const char*>(wave), sizeof(wave));
    audio.close();
    Check(!audio.fail(), "Could not write an isolated voice reference.");
    revia::speech::VoicePresetStore voices(voiceRoot);
    std::string error;
    for (const auto& id : {std::string("alpha"), std::string("beta")})
    {
        revia::speech::VoicePreset preset;
        preset.id = "voice-" + id;
        preset.name = preset.description = preset.referenceText = "Profile fixture " + id;
        preset.referenceAudioPath = reference.string();
        Check(voices.Save(preset, error) && voices.Assign(id, preset.id, error), error);
    }
}

void Probe(ReviaSession& session, Backend& backend, const std::string& marker,
    const std::string& prompt, const std::string& obligation, double temperature, int tokens)
{
    const auto result = session.Submit("Compare maple and oak leaves. " + marker);
    Check(result.succeeded, "The profile conversation failed: " + result.reason);
    const auto request = backend.RequestFor(marker);
    Check(std::abs(request.at("temperature").get<double>() - temperature) < 0.001 &&
        request.at("max_tokens").get<int>() == tokens, "The actual generation request used stale sampling settings.");
    std::string system;
    for (const auto& message : request.at("messages"))
        if (message.value("role", "") == "system") system += message.at("content").get<std::string>();
    Check(system.find(prompt) != std::string::npos &&
        system.find("Answer posture: " + obligation + ".") != std::string::npos,
        "The actual generation request used a stale profile prompt or answer obligation.");
}

void TestActivationOwners()
{
    ScopedTestDirectory directory;
    Backend backend;
    Configure(directory.root, backend.port);
    WorkingDirectory cwd(directory.root);
    revia::core::PreferenceStore preferences;
    std::string speaker;
    float earned = 0.0F;
    {
        ReviaSession session;
        Check(session.Start(), "The profile session did not start.");
        const auto checkOwner = [&](const std::string& id, float baseline)
        {
            Check(session.ProfileStudio().activeProfileId == id && session.VoiceStudio().activeProfile == id &&
                Access::ActiveVoiceDirectory(session) == directory.root / "RuntimeData/Voices" / ("voice-" + id),
                "Session and speech owners disagree on the selected file-stem profile.");
            const auto development = session.CurrentDevelopment();
            Check(std::abs(development.base[Trait::Patience] - baseline) < 0.001F &&
                std::abs(development.delta[Trait::Patience] - earned) < 0.001F,
                "Profile activation skipped its baseline or reset earned identity drift.");
            if (!speaker.empty()) Check(session.CurrentRelationship().entityId == speaker,
                "Profile activation changed the current local person.");
        };
        checkOwner("alpha", 0.2F);
        Probe(session, backend, "startup-probe", "Profile alpha marker.", "reliable", 0.2, 137);
        Access::RememberIdentity(session, "Quentin");
        Check(session.Submit("my name is Quentin").succeeded, "Could not select a local person.");
        speaker = session.CurrentRelationship().entityId;
        earned = session.CurrentDevelopment().delta[Trait::Patience];
        const auto held = session.CurrentPreferences();
        const auto authority = session.Capabilities();

        Check(session.ActivateProfile("beta").succeeded, "UI profile activation failed.");
        checkOwner("beta", 0.8F);
        Check(preferences.Load().at("activeProfile") == "beta", "UI selection was not saved.");
        Probe(session, backend, "ui-probe", "Profile beta marker.", "character first", 0.8, 211);
        const auto seeded = session.CurrentPreferences();
        for (const auto& original : held)
        {
            const auto found = std::find_if(seeded.begin(), seeded.end(), [&](const auto& item)
            { return item.subject == original.subject; });
            Check(found != seeded.end() && found->strength == original.strength &&
                found->evidenceCount == original.evidenceCount,
                "A new profile overwrote an existing preference or its evidence.");
        }
        Check(std::any_of(seeded.begin(), seeded.end(), [](const auto& item)
            { return item.subject == "fixture botany"; }), "A new declared preference was not seeded.");

        Check(session.Submit("/profile alpha").succeeded, "CLI profile activation failed.");
        checkOwner("alpha", 0.2F);
        Check(preferences.Load().at("activeProfile") == "alpha", "CLI selection was not saved.");
        Probe(session, backend, "cli-probe", "Profile alpha marker.", "reliable", 0.2, 137);
        Check(!session.ActivateProfile("missing").succeeded && !session.Submit("/profile ../escape").succeeded,
            "Invalid selection was admitted.");
        checkOwner("alpha", 0.2F);

        // A real store admission failure preserves the old saved file and every live owner.
        const auto blockedWrite = directory.root / "RuntimeData/Preferences/preferences.json.tmp";
        std::filesystem::create_directory(blockedWrite);
        const auto uiFailure = session.ActivateProfile("beta");
        const auto cliFailure = session.Submit("/profile beta");
        std::filesystem::remove(blockedWrite);
        Check(!uiFailure.succeeded && !cliFailure.succeeded &&
            uiFailure.message == cliFailure.text && preferences.Load().at("activeProfile") == "alpha",
            "UI and CLI did not reject an unsaved selection consistently.");
        checkOwner("alpha", 0.2F);

        backend.block = true;
        auto turn = std::async(std::launch::async, [&] { return session.Submit("Compare leaves. blocked-profile-turn"); });
        const bool entered = Until([&] { return backend.entered.load(); });
        const auto busyActivation = session.ActivateProfile("beta");
        backend.block = false;
        const auto completed = turn.get();
        Check(entered && !busyActivation.succeeded && completed.succeeded,
            "Profile selection was allowed to cross an active generation.");
        checkOwner("alpha", 0.2F);

        Check(session.Submit("/profile beta").succeeded, "Could not select the edited profile.");
        ProfileSummary edit;
        edit.id = "beta"; edit.displayName = "Edited Beta"; edit.systemPrompt = "Edited beta marker.";
        edit.memoryEnabled = false; edit.answerObligation = AnswerObligationMode::Balanced;
        // Removing both overrides must restore configured generation defaults.
        Check(session.SaveProfile(edit).succeeded, "Active profile save failed.");
        checkOwner("beta", 0.8F);
        Probe(session, backend, "save-probe", "Edited beta marker.", "balanced", 0.45, 321);
        Check(session.Submit("/profile beta").succeeded, "Same-profile reload failed.");
        checkOwner("beta", 0.8F);
        const auto after = session.Capabilities();
        Check(after.mode == authority.mode && after.approvedRoots == authority.approvedRoots &&
            after.approvedApplications == authority.approvedApplications &&
            after.approvedControls == authority.approvedControls &&
            after.autoApproveRiskThrough == authority.autoApproveRiskThrough &&
            after.internet.enabled == authority.internet.enabled && after.camera.enabled == authority.camera.enabled,
            "Profile selection changed capability authority.");
        session.Stop();
    }
    {
        ReviaSession restarted;
        Check(restarted.Start() && restarted.ProfileStudio().activeProfileId == "beta" &&
            restarted.VoiceStudio().activeProfile == "beta" &&
            Access::ActiveVoiceDirectory(restarted).filename() == "voice-beta" &&
            std::abs(restarted.CurrentDevelopment().base[Trait::Patience] - 0.8F) < 0.001F &&
            std::abs(restarted.CurrentDevelopment().delta[Trait::Patience] - earned) < 0.001F,
            "Restart did not restore a consistent saved profile and earned identity.");
        Probe(restarted, backend, "restart-probe", "Edited beta marker.", "balanced", 0.45, 321);
        Check(restarted.Submit("/set activeProfile alpha").succeeded &&
            restarted.SetPreference("speech.volume", "55").succeeded &&
            restarted.ProfileStudio().activeProfileId == "beta" &&
            restarted.VoiceStudio().activeProfile == "beta" && preferences.Load().at("activeProfile") == "alpha",
            "A next-start preference or unrelated preference partially changed the live profile.");
        Probe(restarted, backend, "pending-preference-probe", "Edited beta marker.", "balanced", 0.45, 321);
        restarted.Stop();
    }
    {
        ReviaSession nextStart;
        Check(nextStart.Start() && nextStart.ProfileStudio().activeProfileId == "alpha" &&
            Access::ActiveVoiceDirectory(nextStart).filename() == "voice-alpha",
            "A startup-only profile preference did not apply on the next start.");
        Probe(nextStart, backend, "next-start-probe", "Profile alpha marker.", "reliable", 0.2, 137);
        nextStart.Stop();
    }
}
}

void RunProfileActivationTests()
{
    TestActivationOwners();
    std::cout << "Profile activation UI, CLI, save, restart and failure owner tests passed.\n";
}
