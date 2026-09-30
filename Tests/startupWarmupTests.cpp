#include "reviaSessionTestAccess.h"

#include <atomic>
#include <fstream>
#include <future>
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>

namespace
{
using namespace std::chrono_literals;
using namespace revia;
using runtime::ReviaSession;
using Access = runtime::ReviaSessionTestAccess;
using intelligence::IntelligenceTier;
using intelligence::ResidencyState;
using tests::Check;
using json = nlohmann::json;

template<class Predicate> bool Until(Predicate predicate)
{
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < end)
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

class WorkingDirectory
{
public:
    explicit WorkingDirectory(const std::filesystem::path& root)
        : previous(std::filesystem::current_path()) { std::filesystem::current_path(root); }
    ~WorkingDirectory() { std::filesystem::current_path(previous); }
private:
    std::filesystem::path previous;
};

class Backend
{
public:
    Backend()
    {
        server.Get("/health", [this](const auto&, auto& response)
        {
            response.status = healthy ? 200 : 503;
            response.set_content(R"({"status":"ok","slots_idle":1})", "application/json");
        });
        server.Get("/v1/models", [](const auto&, auto& response)
        { response.set_content(R"({"data":[{"id":"fixture-main"}]})", "application/json"); });
        server.Get("/props", [](const auto&, auto& response)
        { response.set_content(R"({"total_slots":1,"default_generation_settings":{"n_ctx":8192}})", "application/json"); });
        server.Post("/v1/chat/completions", [this](const auto& request, auto& response)
        {
            const auto body = json::parse(request.body);
            if (body.value("max_tokens", 0) != 1 || body.value("stream", false)) badRequest = true;
            ++posts;
            Until([&] { return !hold.load(); });
            response.status = fail ? 500 : 200;
            response.set_content(R"({"choices":[{"message":{"role":"assistant","content":"ready"},"finish_reason":"stop"}]})", "application/json");
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind startup fixture backend.");
        worker = std::jthread([this] { server.listen_after_bind(); });
        Check(Until([&] { return server.is_running(); }), "Startup fixture did not listen.");
    }
    ~Backend() { hold = false; server.stop(); worker.join(); }
    int port = 0;
    std::atomic<int> posts{0};
    std::atomic<bool> healthy{true}, hold{false}, fail{false}, badRequest{false};
private:
    httplib::Server server;
    std::jthread worker;
};

void Write(const std::filesystem::path& path, const json& value)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << value.dump(2);
    output.close();
    Check(!output.fail(), "Could not write startup fixture configuration.");
}

void Configure(const std::filesystem::path& root, int port)
{
    Write(root / "Config/settings.json", {
        {"activeProfile", "fixture"},
        {"llm", {{"backend", "LLamaCpp"}, {"host", "127.0.0.1"}, {"port", port},
            {"modelName", "fixture-main"}, {"autoStartServer", false}, {"visionEnabled", false},
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
        {"resources", {{"startupSampleSeconds", 0}}}
    });
    Write(root / "Config/Profiles/fixture.json", {
        {"id", "fixture"}, {"systemPrompt", "You are Revia."}, {"memoryEnabled", false}
    });
}

// Catches the healthy endpoint early return that bypasses graph preparation.
void HealthyMainPreparesOnce()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    ReviaSession session;
    Access::ConfigureStartupBrains(session, backend.port);
    Check(Access::EnsureBrain(session, IntelligenceTier::Main), "Healthy Main was unavailable.");
    Check(backend.posts == 1, "Healthy external Main must receive one warmup POST; observed " + std::to_string(backend.posts.load()));
    Check(!backend.badRequest, "Preparation must use one token without streaming.");
    Check(Access::BrainState(session, IntelligenceTier::Main) == ResidencyState::Warm, "Successful preparation did not publish Warm.");
    Access::ApplyBrainProfile(session);
    Check(Access::EnsureBrain(session, IntelligenceTier::Main), "Repeated Main health failed.");
    Check(backend.posts == 1, "Routine checks/profile changes repeated Main preparation.");
}

// Catches config flags being treated as evidence that inference actually succeeded.
void StartupFailureStaysCold()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    backend.fail = true;
    Configure(directory.root, backend.port);
    ReviaSession session;
    Check(session.Start(), "Optional preparation failure prevented session startup.");
    Check(Access::BrainState(session, IntelligenceTier::Main) == ResidencyState::Cold,
        "Failed preparation was falsely reported Warm by full Start.");
    Check(backend.posts == 1, "Full Start did not attempt bounded Main preparation.");
    Check(Access::EnsureBrain(session, IntelligenceTier::Main), "Preparation failure removed healthy availability.");
    Check(backend.posts == 1, "Ordinary preparation failure was hammered on a routine check.");
    session.Stop();
    backend.fail = false;
    Check(session.Start(), "Fresh startup failed.");
    Check(backend.posts == 2, "Fresh startup did not reset preparation attempts.");
    Check(Access::BrainState(session, IntelligenceTier::Main) == ResidencyState::Warm, "Fresh successful startup stayed Cold.");
}

void OptionalTierFlags()
{
    for (const auto tier : {IntelligenceTier::Fast, IntelligenceTier::Expert})
        for (const bool warm : {false, true})
        {
            tests::ScopedTestDirectory directory;
            WorkingDirectory cwd(directory.root);
            Backend backend;
            ReviaSession session;
            Access::ConfigureStartupBrains(session, backend.port, warm, warm);
            Check(Access::EnsureBrain(session, tier), "Healthy optional tier was unavailable.");
            Check(backend.posts == (warm ? 1 : 0), "Optional tier ignored warmAtStartup.");
            Check(Access::BrainState(session, tier) == (warm ? ResidencyState::Warm : ResidencyState::Cold),
                "Optional tier readiness disagrees with actual preparation.");
            Check(Access::EnsureBrain(session, tier), "Repeated optional tier health failed.");
            Check(backend.posts == (warm ? 1 : 0), "Optional tier preparation was repeated.");
            Access::VirtuallyUnloadBrain(session, tier);
            Check(Access::EnsureBrain(session, tier), "External endpoint did not recover from virtual unload.");
            Check(backend.posts == (warm ? 1 : 0), "Virtual unload repeated preparation of an external endpoint.");
        }
}

void OptionalFailuresStayCold()
{
    for (const auto tier : {IntelligenceTier::Fast, IntelligenceTier::Expert})
    {
        tests::ScopedTestDirectory directory;
        WorkingDirectory cwd(directory.root);
        Backend backend;
        backend.fail = true;
        ReviaSession session;
        Access::ConfigureStartupBrains(session, backend.port);
        Check(Access::EnsureBrain(session, tier), "Optional preparation failure removed availability.");
        Check(backend.posts == 1, "Optional tier did not attempt preparation.");
        Check(Access::BrainState(session, tier) == ResidencyState::Cold, "Failed optional preparation was marked Warm.");
        Check(Access::EnsureBrain(session, tier), "Repeated failed optional preparation removed availability.");
        Check(backend.posts == 1, "Optional failure was hammered on a routine check.");
    }
}

void CancellationIsPromptAndRetryable()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    backend.hold = true;
    ReviaSession session;
    Access::ConfigureStartupBrains(session, backend.port);
    std::stop_source cancellation;
    auto operation = std::async(std::launch::async, [&]
    { return Access::EnsureBrain(session, IntelligenceTier::Main, cancellation.get_token()); });
    const bool entered = Until([&] { return backend.posts.load() == 1; });
    cancellation.request_stop();
    const bool prompt = operation.wait_for(1500ms) == std::future_status::ready;
    backend.hold = false;
    const bool available = operation.get();
    Check(entered, "Main never entered cancellable preparation.");
    Check(prompt, "Cancellation waited for the held backend response.");
    Check(available, "Cancelled optional preparation removed healthy availability.");
    Check(Access::BrainState(session, IntelligenceTier::Main) == ResidencyState::Cold, "Cancelled preparation was marked Warm.");
    Check(Access::EnsureBrain(session, IntelligenceTier::Main), "Retry after cancellation failed.");
    Check(backend.posts == 2, "Cancellation was cached instead of allowing a retry.");
    Check(Access::BrainState(session, IntelligenceTier::Main) == ResidencyState::Warm, "Retry did not publish Warm.");
}

void LostBackendResetsPreparation()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    ReviaSession session;
    Access::ConfigureStartupBrains(session, backend.port);
    Check(Access::EnsureBrain(session, IntelligenceTier::Main), "Main unavailable before loss.");
    backend.healthy = false;
    Check(!Access::EnsureBrain(session, IntelligenceTier::Main), "Lost backend remained available.");
    Check(Access::BrainState(session, IntelligenceTier::Main) == ResidencyState::Failed, "Observed backend loss retained Warm residency.");
    backend.healthy = true;
    Check(Access::EnsureBrain(session, IntelligenceTier::Main), "Recovered backend was unavailable.");
    Check(backend.posts == 2, "Recovered backend reused stale preparation.");
    Check(Access::BrainState(session, IntelligenceTier::Main) == ResidencyState::Warm, "Recovered preparation stayed cold.");
}
} // namespace

void RunStartupWarmupTests()
{
    HealthyMainPreparesOnce();
    StartupFailureStaysCold();
    OptionalTierFlags();
    OptionalFailuresStayCold();
    CancellationIsPromptAndRetryable();
    LostBackendResetsPreparation();
    std::cout << "Startup warmup tests passed.\n";
}
