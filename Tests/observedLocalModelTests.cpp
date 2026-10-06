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
        server.Post("/apply-template", [](const auto& request, auto& response)
            {
                response.status = 201;
                response.set_content(request.body, "application/json");
            });
        server.Post("/tokenize", [](const auto&, auto& response)
            {
                response.status = 422;
                response.set_content(R"({"error":"bad tokens"})", "application/json");
            });
        server.Post("/v1/chat/completions", [](const auto&, auto& response)
            {
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

  private:
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
        Check(rendered && rendered->status == 201 && rendered->body == body, "Template rendering must preserve request body and upstream status.");
        Check(rendered->get_header_value("Content-Type").find("application/json") == 0, "Template response must preserve its content type.");
        const auto invalid = client.Post("/tokenize", "{}", "application/json");
        Check(invalid && invalid->status == 422 && invalid->body == R"({"error":"bad tokens"})", "Tokenization must preserve backend errors.");
        Check(observed.Requests().empty() && observed.RawRequests().empty() && observed.Responses().empty(), "Accounting calls must not shift completion evaluation indices.");
        const auto chat = client.Post("/v1/chat/completions", R"({"messages":[]})", "application/json");
        Check(chat && chat->status == 200 && observed.Requests().size() == 1, "Completion traffic must still be observed.");
        Check(observed.RawRequests().front().at("requestIndex") == 0, "First completion must retain index zero.");
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
