// Opt-in live evidence harness, included by taskContractSessionTests.cpp.
class LiveWorkerProxy
{
public:
    LiveWorkerProxy(const nlohmann::json& configuration, const std::filesystem::path& evidence, unsigned seed)
        : evidence(evidence), host(configuration.at("host")), upstreamPort(configuration.at("port")), seed(seed)
    {
        httplib::Client upstream(host, upstreamPort);
        const auto models = upstream.Get("/v1/models");
        Check(models && models->status == 200, "live provider model inventory available");
        std::ofstream(evidence / "models.json") << models->body;
        model = nlohmann::json::parse(models->body).at("data").at(0).at("id").get<std::string>();
        server.Get("/health", [this](const auto&, auto& response) { ForwardGet("/health", response); });
        server.Get("/v1/models", [this](const auto&, auto& response) { ForwardGet("/v1/models", response); });
        server.Post("/v1/chat/completions", [this](const httplib::Request& request, httplib::Response& response)
        {
            const auto call = calls.fetch_add(1);
            const auto prefix = this->evidence / ("wire-" + std::to_string(call));
            std::ofstream(prefix.string() + "-request.json") << request.body;
            auto forwarded = nlohmann::json::parse(request.body);
            forwarded["model"] = model;
            forwarded["seed"] = this->seed;
            std::ofstream(prefix.string() + "-forwarded.json") << forwarded.dump(2);
            httplib::Client client(host, upstreamPort);
            client.set_read_timeout(90, 0);
            client.set_write_timeout(30, 0);
            const auto result = client.Post("/v1/chat/completions", forwarded.dump(), "application/json");
            if (!result)
            {
                response.status = 502;
                response.set_content("{\"error\":\"upstream unavailable\"}", "application/json");
                std::ofstream(prefix.string() + "-transport-error.txt") << httplib::to_string(result.error());
                return;
            }
            std::ofstream(prefix.string() + "-response.json") << result->body;
            bool toolProposal = false;
            try
            {
                const auto envelope = nlohmann::json::parse(result->body);
                const auto content = nlohmann::json::parse(envelope.at("choices").at(0).at("message").at("content").get<std::string>());
                toolProposal = content.contains("tool") && content.at("tool").is_object();
            }
            catch (const nlohmann::json::exception&) {}
            bool expected = false;
            if (toolProposal)
            {
                if (proposed.compare_exchange_strong(expected, true))
                    firstProposal.store(call);
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (!release.load() && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            response.status = result->status;
            response.set_content(result->body, "application/json");
        });
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "live proxy bound");
        worker = std::jthread([this] { server.listen_after_bind(); });
    }

    ~LiveWorkerProxy()
    {
        release.store(true);
        server.stop();
        if (worker.joinable()) worker.join();
    }

    int port = 0;
    std::atomic<bool> proposed{false}, release{false};
    std::atomic<unsigned> calls{0}, firstProposal{0};

private:
    void ForwardGet(const char* path, httplib::Response& response)
    {
        httplib::Client client(host, upstreamPort);
        const auto result = client.Get(path);
        response.status = result ? result->status : 502;
        response.set_content(result ? result->body : "{}", "application/json");
    }

    std::filesystem::path evidence;
    std::string host, model;
    int upstreamPort;
    unsigned seed;
    httplib::Server server;
    std::jthread worker;
};

int RunLiveWorker(const std::filesystem::path& manifest, const std::filesystem::path& evidence,
    const std::string& scenario, unsigned seed)
{
    std::filesystem::create_directories(evidence);
    std::ifstream manifestInput(manifest);
    const auto configuration = nlohmann::json::parse(manifestInput);
    const auto task = configuration.at("tasks").at(scenario);
    tests::ScopedTestDirectory directory;
    const auto file = directory.root / "result.txt";
    std::string objective = task.at("objective");
    const auto marker = objective.find("<fixture>/result.txt");
    Check(marker != std::string::npos, "frozen objective has its declared isolated runtime path");
    objective.replace(marker, std::string("<fixture>/result.txt").size(), actions::PathToUtf8(file));
    std::ofstream(evidence / "admitted-objective.json") << nlohmann::json{{"objective", objective},
        {"manifestDigest", audit::ContentDigest(configuration.dump())}, {"seed", seed}, {"scenario", scenario}}.dump(2);
    LiveWorkerProxy backend(configuration, evidence, seed);
    runtime::ReviaSession session(runtime::CompanionPaths(directory.root, {"live-worker", "Live worker", "assistant", false}));
    using Access = runtime::ReviaSessionTestAccess;
    (void)Access::Admit(session, "local:alpha", "live-originating-turn");
    std::mutex dispatchMutex;
    nlohmann::json dispatches = nlohmann::json::array();
    Access::ConfigureWorker(session, backend.port, directory.root, [&](const auto& action, bool before)
    {
        if (!before) return;
        nlohmann::json entry = {{"id", action.id}, {"type", actions::ToString(action.type)}};
        std::string serialized;
        if (action.taskContract && core::SerializeTaskContract(*action.taskContract, serialized))
            entry["contract"] = nlohmann::json::parse(serialized);
        std::lock_guard lock(dispatchMutex);
        dispatches.push_back(std::move(entry));
    });
    std::string error;
    const auto startedAt = std::chrono::steady_clock::now();
    const bool admitted = session.StartAgentWorkflow(objective, runtime::AgentProviderMode::LocalWithTools, error);
    bool switched = false, timedOut = false;
    if (admitted)
    {
        const auto until = startedAt + std::chrono::seconds(180);
        while (std::chrono::steady_clock::now() < until)
        {
            if (backend.proposed.load() && !backend.release.load())
            {
                if (scenario == "participant")
                {
                    (void)Access::Admit(session, "local:beta", "live-unrelated-turn");
                    switched = true;
                }
                backend.release.store(true);
            }
            session.PollBackgroundEvents();
            const auto state = session.AgentWorkflowSnapshot().state;
            if (state != agents::WorkflowState::Running && state != agents::WorkflowState::Waiting &&
                state != agents::WorkflowState::Queued && state != agents::WorkflowState::AwaitingAcceptance)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        timedOut = std::chrono::steady_clock::now() >= until;
        if (timedOut) session.CancelAgentWorkflow();
        backend.release.store(true);
        Access::JoinWorker(session);
    }
    const auto snapshot = session.AgentWorkflowSnapshot();
    const bool exists = std::filesystem::exists(file);
    std::ifstream output(file, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(output)), std::istreambuf_iterator<char>());
    const bool exact = exists && contents == task.at("expected_text").get<std::string>();
    for (const auto& [source, destination] : std::vector<std::pair<std::filesystem::path, std::string>>{
        {directory.root / "worker-journal.jsonl", "native-journal.jsonl"},
        {session.Paths().Resolve("RuntimeData/Agents/tool-scope.json"), "admission.json"},
        {session.Paths().Resolve("RuntimeData/Agents/workflow.json"), "workflow.json"}})
        if (std::filesystem::exists(source))
            std::filesystem::copy_file(source, evidence / destination, std::filesystem::copy_options::overwrite_existing);
    unsigned attempted = 0, succeeded = 0;
    std::ifstream journal(directory.root / "worker-journal.jsonl");
    for (std::string line; std::getline(journal, line);)
    {
        const auto record = nlohmann::json::parse(line);
        if (record.value("record_type", "") == "result")
        {
            attempted += record.value("attempted", false) ? 1 : 0;
            succeeded += record.value("succeeded", false) ? 1 : 0;
        }
    }
    const bool passed = admitted && backend.proposed.load() && !timedOut &&
        (scenario == "participant" ? switched && !exists && attempted == 0 : exact && snapshot.state == agents::WorkflowState::Accepted);
    const nlohmann::json result = {{"passed", passed}, {"admitted", admitted}, {"admission_error", error},
        {"scenario", scenario}, {"seed", seed}, {"proposed", backend.proposed.load()}, {"switched", switched},
        {"provider_calls", backend.calls.load()}, {"first_proposal_index", backend.firstProposal.load()},
        {"timed_out", timedOut}, {"state", agents::ToString(snapshot.state)}, {"file_exists", exists},
        {"exact_content", exact}, {"native_attempted", attempted}, {"native_succeeded", succeeded},
        {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt).count()}};
    std::ofstream(evidence / "dispatch-contracts.json") << dispatches.dump(2);
    std::ofstream(evidence / "result.json") << result.dump(2);
    std::ofstream(evidence / "artifact.txt") << session.AgentWorkflowResult();
    if (exists) std::ofstream(evidence / "native-content.txt", std::ios::binary) << contents;
    std::cout << result.dump(2) << '\n';
    return passed ? 0 : 1;
}
