#include "../Tools/Quality/observedLocalModel.h"
#include "../Tools/Quality/answerGateExperiment.h"

#include <iostream>

namespace
{
void Check(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void Check(const bool condition, const std::string& message)
{
    Check(condition, message.c_str());
}

class Upstream
{
  public:
    explicit Upstream(const std::string& requiredKey = {})
    {
        server.set_pre_routing_handler([requiredKey](const auto& request, auto& response)
        {
            if (!requiredKey.empty() && request.get_header_value("Authorization") != "Bearer " + requiredKey)
            {
                response.status = 401;
                response.set_content(R"({"error":"unauthorized"})", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server.Get("/v1/models", [](const auto&, auto& response)
        { response.set_content(R"({"data":[{"id":"fixture"}]})", "application/json"); });
        server.Post("/apply-template",
            [](const auto& request, auto& response)
            {
                response.status = 201;
                response.set_content(request.body, "application/json");
            });
        server.Post("/tokenize",
            [](const auto&, auto& response)
            {
                response.status = 422;
                response.set_content(R"({"error":"bad tokens"})", "application/json");
            });
        server.Post("/v1/chat/completions",
            [this](const auto& request, auto& response)
            {
                const std::lock_guard lock(mutex);
                completions.push_back(request.body);
                response.set_content(request.body.find("fixtureSecret") != std::string::npos ? request.body : R"({"choices":[]})", "application/json");
            });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Fixture must bind loopback.");
        worker = std::jthread([this] { server.listen_after_bind(); });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!server.is_running() && std::chrono::steady_clock::now() < until)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~Upstream()
    {
        Stop();
    }

    void Stop()
    {
        server.stop();
        if (worker.joinable())
        {
            worker.join();
        }
    }

    int port = 0;

    [[nodiscard]] std::vector<std::string> Completions() const
    {
        const std::lock_guard lock(mutex);
        return completions;
    }

  private:
    mutable std::mutex mutex;
    std::vector<std::string> completions;
    httplib::Server server;
    std::jthread worker;
};
}

int main()
{
    try
    {
        const auto refuses = [](const auto& action)
        {
            try { action(); } catch (const std::runtime_error&) { return true; }
            return false;
        };
        for (const auto& value : {std::string{}, std::string("abc\r\ndef"), std::string("a b"), std::string(4097, 'x')})
            Check(refuses([&] { revia::quality::LocalModelCredentialSource::Validate(value); }),
                "Empty, oversized, or header-breaking credentials must fail closed.");
        revia::quality::LocalModelCredentialSource ambiguous;
        ambiguous.file = "not-opened";
        ambiguous.environment = "not-read";
        Check(refuses([&] { (void)ambiguous.Read(); }), "Ambiguous secret sources must be refused before reading either source.");
        const auto credentialDirectory = std::filesystem::temp_directory_path() /
            ("revia-credential-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(credentialDirectory);
        revia::quality::LocalModelCredentialSource credential;
        credential.file = credentialDirectory / "fixture.key";
        { std::ofstream file(credential.file, std::ios::binary); file << "fixture-token\r\n"; }
        Check(credential.Read() == "fixture-token", "A file credential may end in a conventional newline.");
        { std::ofstream file(credential.file, std::ios::binary); file << std::string(4096, 'x') << "\r\nsuffix"; }
        Check(refuses([&] { (void)credential.Read(); }), "Oversized files must never silently truncate to a valid credential.");
        std::filesystem::remove_all(credentialDirectory);
        revia::quality::AnswerGateExperiment experiment;
        experiment.ValidateBaselineRequest(true, true, false);
        Check(refuses([&] { experiment.ValidateBaselineRequest(true, false, false); }) &&
                  refuses([&] { experiment.ValidateBaselineRequest(true, true, true); }),
            "Deterministic-only and subset runs cannot be baseline artifacts.");
        experiment.statePacket = false;
        Check(refuses([&] { experiment.ValidateBaselineRequest(true, true, false); }),
            "An ablation must not become the canonical baseline.");
        experiment.ValidateBaselineRequest(false, true, false);
        {
            const std::string key = "explicit-evaluation-fixture-key";
            Upstream protectedUpstream(key);
            revia::quality::ObservedLocalModel protectedObserver(protectedUpstream.port, key);
            httplib::Client unauthenticated("127.0.0.1", protectedObserver.port);
            for (const auto* path : {"/v1/models", "/unrecognized"})
            {
                const auto denied = unauthenticated.Get(path);
                Check(denied && denied->status == 401, "Protected proxy must not expose an unauthenticated bridge.");
            }
            unauthenticated.set_default_headers({{"Authorization", "Bearer incorrect"}});
            const auto wrongKey = unauthenticated.Post("/v1/chat/completions", "{}", "application/json");
            // Checked apart: this failed in Windows CI without saying which part did.
            Check(static_cast<bool>(wrongKey), "A wrong-credential request got no response at all: " +
                                                   std::string(wrongKey ? "" : httplib::to_string(wrongKey.error())) + ".");
            Check(wrongKey->status == 401, "Wrong credentials got status " + std::to_string(wrongKey->status) + " instead of 401.");
            Check(protectedObserver.Requests().empty(), "Wrong credentials must be refused before forwarding or observation.");
            // A refusal that closed the socket over the unread body reset the connection on
            // Windows, so the client often never read its 401. Each of these must read it.
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                httplib::Client wrong("127.0.0.1", protectedObserver.port);
                wrong.set_default_headers({{"Authorization", "Bearer incorrect"}});
                const auto refused = wrong.Post("/v1/chat/completions", std::string(4096, ' ') + "{}", "application/json");
                Check(refused && refused->status == 401,
                    "A refused request with a body did not reliably return its 401: " +
                        (refused ? "status " + std::to_string(refused->status) : httplib::to_string(refused.error())) + ".");
            }
            Check(protectedObserver.Requests().empty(), "A refused request with a body reached the upstream.");
            httplib::Client authorized("127.0.0.1", protectedObserver.port);
            authorized.set_default_headers({{"Authorization", "Bearer " + key}});
            const auto models = authorized.Get("/v1/models");
            Check(models && models->status == 200,
                "Explicit evaluation authentication must reach upstream model discovery.");
            const auto templateReply = authorized.Post("/apply-template", "{}", "application/json");
            const auto tokenReply = authorized.Post("/tokenize", "{}", "application/json");
            Check(templateReply && templateReply->status == 201 && tokenReply && tokenReply->status == 422,
                "Authenticated accounting must preserve upstream status.");
            const std::string echo = nlohmann::json{{"messages", nlohmann::json::array()}, {"fixtureSecret", key}}.dump();
            const auto completion = authorized.Post("/v1/chat/completions", echo, "application/json");
            Check(completion && completion->status == 200 && protectedUpstream.Completions().front() == echo,
                "Authenticated completion must preserve original request bytes.");
            Check(completion->body.find(key) == std::string::npos,
                "An upstream-reflected credential must not enter runtime replies or diagnostic logs.");
            Check(nlohmann::json(protectedObserver.Requests()).dump().find(key) == std::string::npos &&
                      nlohmann::json(protectedObserver.RawRequests()).dump().find(key) == std::string::npos &&
                      nlohmann::json(protectedObserver.Responses()).dump().find(key) == std::string::npos,
                "Explicit credentials must be redacted from captured traffic if reflected in content.");
            Check(protectedObserver.Requests().size() == 1, "Accounting calls must not enter authenticated completion indices.");
        }
        Upstream upstream;
        revia::quality::ObservedLocalModel observed(upstream.port);
        httplib::Client client("127.0.0.1", observed.port);
        client.set_read_timeout(10);
        const std::string body = R"({"messages":[{"role":"user","content":"a & b"}],"chat_template_kwargs":{"enable_thinking":false}})";
        const auto rendered = client.Post("/apply-template", body, "application/json");
        Check(rendered && rendered->status == 201 && rendered->body == body,
            "Template rendering must preserve request body and upstream status.");
        Check(
            rendered->get_header_value("Content-Type").find("application/json") == 0, "Template response must preserve its content type.");
        const auto invalid = client.Post("/tokenize", "{}", "application/json");
        Check(invalid && invalid->status == 422 && invalid->body == R"({"error":"bad tokens"})",
            "Tokenization must preserve backend errors.");
        Check(observed.Requests().empty() && observed.RawRequests().empty() && observed.Responses().empty(),
            "Accounting calls must not shift completion evaluation indices.");
        const auto chat = client.Post("/v1/chat/completions", R"({"messages":[]})", "application/json");
        Check(chat && chat->status == 200 && observed.Requests().size() == 1, "Completion traffic must still be observed.");
        Check(observed.RawRequests().front().at("requestIndex") == 0, "First completion must retain index zero.");
        Check(
            upstream.Completions().front() == R"({"messages":[]})", "Default observer must forward exact completion bytes without a seed.");
        observed.SetCompletionSeed(47);
        const auto seeded = client.Post("/v1/chat/completions", R"({"messages":[],"seed":999,"temperature":0.8})", "application/json");
        Check(seeded && seeded->status == 200, "Seeded completion must reach upstream.");
        const auto received = nlohmann::json::parse(upstream.Completions().back());
        Check(received.at("seed") == 47 && received.at("temperature") == 0.8,
            "Frozen seed must reach the real upstream without changing temperature.");
        const auto traffic = observed.RawRequests().back();
        Check(nlohmann::json::parse(traffic.at("body").get<std::string>()).at("seed") == 999 &&
                  nlohmann::json::parse(traffic.at("forwardedBody").get<std::string>()).at("seed") == 47,
            "Traffic must retain original and actually seeded request bodies.");
        const auto sent = upstream.Completions().size();
        const auto malformed = client.Post("/v1/chat/completions", "[]", "application/json");
        Check(malformed && malformed->status == 400 && upstream.Completions().size() == sent,
            "Seed injection must refuse invalid envelopes before forwarding an unseeded request.");
        observed.SetCompletionSeed(std::nullopt);
        const std::string explicitSeed = R"({"messages":[],"seed":73})";
        const auto unchanged = client.Post("/v1/chat/completions", explicitSeed, "application/json");
        Check(unchanged && upstream.Completions().back() == explicitSeed, "Clearing override must restore exact default forwarding.");
        const auto directory = std::filesystem::temp_directory_path() /
                               ("revia-observed-seed-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        const auto seedReport = directory / "seed-report";
        observed.SaveTrafficSince(seedReport, 1);
        {
            std::ifstream saved(seedReport.string() + ".requests.json");
            const auto seedTraffic = nlohmann::json::parse(saved);
            Check(seedTraffic.size() == 3 && seedTraffic.front().at("requestIndex") == 1,
                "Per-seed traffic must exclude prior completion requests while preserving all current outcomes.");
            std::ifstream responses(seedReport.string() + ".responses.json");
            const auto seedResponses = nlohmann::json::parse(responses);
            Check(seedResponses.size() == 3 && seedResponses.at(1).at("status") == 400,
                "Per-seed traffic must preserve refused completion results with their correlation indices.");
        }
        std::filesystem::remove_all(directory);
        upstream.Stop();
        const auto unavailable = client.Post("/tokenize", "{}", "application/json");
        Check(unavailable && unavailable->status == 502, "Accounting transport failure must not masquerade as a count.");
        observed.Close();
        std::cout << "ObservedLocalModel accounting forwarding checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
