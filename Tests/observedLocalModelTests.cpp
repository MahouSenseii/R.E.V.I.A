#include "../Tools/Quality/observedLocalModel.h"

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

class Upstream
{
  public:
    Upstream()
    {
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
                response.set_content(R"({"choices":[]})", "application/json");
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
