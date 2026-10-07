#pragma once

#include "Evaluation/campaignManifest.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace revia::quality
{
class ObservedLocalModel
{
  public:
    explicit ObservedLocalModel(const int upstreamPort)
    {
        if (upstreamPort <= 0 || upstreamPort > 65535)
        {
            throw std::runtime_error("The observation proxy needs a valid loopback upstream port.");
        }
        server.Get(".*",
            [this, upstreamPort](const auto& request, auto& response)
            {
                const auto upstream = Client(upstreamPort);
                if (!upstream)
                {
                    response.status = 502;
                    return;
                }
                const auto received = upstream->Get(request.path);
                if (!received)
                {
                    response.status = 502;
                    return;
                }
                response.status = received->status;
                response.set_content(received->body, received->get_header_value("Content-Type"));
            });
        for (const auto* path : {"/apply-template", "/tokenize"})
        {
            server.Post(path,
                [this, upstreamPort](const auto& request, auto& response)
                {
                    // Token accounting must not change the completion indices used by quality reviewers.
                    const auto upstream = Client(upstreamPort);
                    const auto received = upstream ? upstream->Post(request.path, request.body, "application/json") : httplib::Result{};
                    if (!received)
                    {
                        response.status = 502;
                        return;
                    }
                    response.status = received->status;
                    response.set_content(received->body, received->get_header_value("Content-Type"));
                });
        }
        server.Post("/v1/chat/completions",
            [this, upstreamPort](const auto& request, auto& response)
            {
                std::size_t index;
                std::string forwardedBody = request.body;
                {
                    const std::lock_guard lock(mutex);
                    index = requests.size();
                    auto parsed = nlohmann::json::parse(request.body, nullptr, false);
                    if (completionSeed)
                    {
                        if (!parsed.is_object())
                        {
                            response.status = 400;
                            response.set_content(R"({"error":"Seeded completion requires a JSON object."})", "application/json");
                            requests.push_back(nlohmann::json{});
                            rawRequests.push_back({{"requestIndex", index}, {"method", request.method}, {"path", request.path},
                                {"body", request.body}, {"seedInjectionError", true}});
                            responses.push_back({{"requestIndex", index}, {"status", response.status}, {"body", response.body}});
                            return;
                        }
                        parsed["seed"] = *completionSeed;
                        forwardedBody = parsed.dump();
                    }
                    requests.push_back(parsed.is_discarded() ? nlohmann::json{} : std::move(parsed));
                    rawRequests.push_back({{"requestIndex", index}, {"method", request.method}, {"path", request.path},
                        {"body", request.body}, {"forwardedBody", forwardedBody}, {"seedInjected", completionSeed.has_value()}});
                }
                const auto upstream = Client(upstreamPort);
                const auto received = upstream ? upstream->Post(request.path, forwardedBody, "application/json") : httplib::Result{};
                if (!received)
                {
                    const std::lock_guard lock(mutex);
                    responses.push_back({{"requestIndex", index}, {"transportError", true}});
                    response.status = 502;
                    return;
                }
                {
                    const std::lock_guard lock(mutex);
                    responses.push_back({{"requestIndex", index}, {"status", received->status}, {"body", received->body}});
                }
                response.status = received->status;
                response.set_content(received->body, received->get_header_value("Content-Type"));
            });
        server.set_read_timeout(5, 0);
        server.set_write_timeout(5, 0);
        server.new_task_queue = [] { return new httplib::ThreadPool(4, 64); };
        port = server.bind_to_any_port("127.0.0.1");
        if (port <= 0)
        {
            throw std::runtime_error("The observation proxy could not bind loopback.");
        }
        try
        {
            worker = std::jthread([this] { server.listen_after_bind(); });
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!server.is_running() && std::chrono::steady_clock::now() < until)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!server.is_running())
            {
                throw std::runtime_error("The observation proxy did not start.");
            }
        }
        catch (...)
        {
            Close();
            throw;
        }
    }

    ~ObservedLocalModel()
    {
        Close();
    }

    void Close()
    {
        std::vector<std::shared_ptr<httplib::ClientImpl>> pending;
        {
            const std::lock_guard lock(mutex);
            closing = true;
            for (const auto& client : activeClients)
            {
                if (const auto alive = client.lock())
                {
                    pending.push_back(alive);
                }
            }
        }
        server.stop();
        // Stop outstanding loopback reads before joining the listener's worker pool.
        for (const auto& client : pending)
        {
            client->stop();
        }
        if (worker.joinable())
        {
            worker.join();
        }
    }

    [[nodiscard]] std::vector<nlohmann::json> Requests() const
    {
        const std::lock_guard lock(mutex);
        return requests;
    }

    void SetCompletionSeed(std::optional<std::uint64_t> seed)
    {
        const std::lock_guard lock(mutex);
        completionSeed = seed;
    }

    [[nodiscard]] std::vector<nlohmann::json> RawRequests() const
    {
        const std::lock_guard lock(mutex);
        return rawRequests;
    }

    [[nodiscard]] std::vector<nlohmann::json> Responses() const
    {
        const std::lock_guard lock(mutex);
        return responses;
    }

    void SaveTraffic(const std::filesystem::path& reportPath) const
    {
        SaveJson(reportPath.string() + ".requests.json", RawRequests());
        SaveJson(reportPath.string() + ".responses.json", Responses());
    }

    void SaveTrafficSince(const std::filesystem::path& reportPath, std::size_t firstRequestIndex) const
    {
        auto capturedRequests = RawRequests();
        auto capturedResponses = Responses();
        const auto previous = [firstRequestIndex](const auto& item)
        { return item.at("requestIndex").template get<std::size_t>() < firstRequestIndex; };
        std::erase_if(capturedRequests, previous);
        std::erase_if(capturedResponses, previous);
        SaveJson(reportPath.string() + ".requests.json", capturedRequests, true);
        SaveJson(reportPath.string() + ".responses.json", capturedResponses, true);
    }

    int port = 0;

  private:
    class CancellableStream final : public httplib::Stream
    {
      public:
        CancellableStream(const socket_t socket, const std::atomic<bool>& closing) : stream(socket, 0, 100000, 0, 100000), closing(closing)
        {
        }

        bool is_readable() const override
        {
            return Wait([this] { return stream.is_readable(); });
        }

        bool is_writable() const override
        {
            return Wait([this] { return stream.is_writable(); });
        }

        ssize_t read(char* data, const std::size_t size) override
        {
            return Transfer([this, data, size] { return stream.read(data, size); });
        }

        ssize_t write(const char* data, const std::size_t size) override
        {
            return Transfer([this, data, size] { return stream.write(data, size); });
        }

        void get_remote_ip_and_port(std::string& ip, int& port) const override
        {
            stream.get_remote_ip_and_port(ip, port);
        }

        void get_local_ip_and_port(std::string& ip, int& port) const override
        {
            stream.get_local_ip_and_port(ip, port);
        }

        socket_t socket() const override
        {
            return stream.socket();
        }

      private:
        template <typename Operation> bool Wait(Operation operation) const
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(180);
            while (!closing && std::chrono::steady_clock::now() < until)
            {
                if (operation())
                {
                    return true;
                }
                if (!httplib::detail::is_socket_alive(stream.socket()))
                {
                    return false;
                }
            }
            return false;
        }

        template <typename Operation> ssize_t Transfer(Operation operation)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(180);
            while (!closing && std::chrono::steady_clock::now() < until)
            {
                const auto transferred = operation();
                if (transferred >= 0)
                {
                    return transferred;
                }
                if (!httplib::detail::is_socket_alive(stream.socket()))
                {
                    return -1;
                }
            }
            return -1;
        }

        httplib::detail::SocketStream stream;
        const std::atomic<bool>& closing;
    };

    class CancellableClient final : public httplib::ClientImpl
    {
      public:
        CancellableClient(const int port, const std::atomic<bool>& closing) : httplib::ClientImpl("127.0.0.1", port), closing(closing)
        {
        }

      protected:
        bool process_socket(const Socket& socket, std::function<bool(httplib::Stream&)> callback) override
        {
            CancellableStream stream(socket.sock, closing);
            return callback(stream);
        }

      private:
        const std::atomic<bool>& closing;
    };

    std::shared_ptr<httplib::ClientImpl> Client(const int upstreamPort)
    {
        const std::lock_guard lock(mutex);
        if (closing)
        {
            return {};
        }
        // Short readiness waits allow Windows shutdown to interrupt the original read budget.
        auto client = std::make_shared<CancellableClient>(upstreamPort, closing);
        client->set_connection_timeout(5, 0);
        client->set_read_timeout(180, 0);
        client->set_write_timeout(180, 0);
        std::erase_if(activeClients, [](const auto& prior) { return prior.expired(); });
        activeClients.push_back(client);
        return client;
    }

    static void SaveJson(const std::filesystem::path& path, const nlohmann::json& value, bool immutable = false)
    {
        if (immutable)
        {
            std::string error;
            if (!evaluation::WriteEvaluationArtifactOnce(path, value.dump(2) + '\n', error))
                throw std::runtime_error(error);
            return;
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << value.dump(2) << '\n';
        output.flush();
        output.close();
        if (!output.good())
        {
            throw std::runtime_error("Cannot retain the exact observed loopback traffic.");
        }
    }

    mutable std::mutex mutex;
    std::atomic<bool> closing{false};
    std::optional<std::uint64_t> completionSeed;
    std::vector<nlohmann::json> requests;
    std::vector<nlohmann::json> rawRequests;
    std::vector<nlohmann::json> responses;
    std::vector<std::weak_ptr<httplib::ClientImpl>> activeClients;
    httplib::Server server;
    std::jthread worker;
};
} // namespace revia::quality
