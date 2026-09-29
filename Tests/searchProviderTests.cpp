#include "testSupport.h"

#include "Actions/actionTypes.h"
#include "Internet/internetBackend.h"
#include "Internet/internetSearchExecutor.h"
#include "Internet/webReader.h"
#include "LLM/httpsTransport.h"

#include <chrono>
#include <httplib.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

// Search through a provider of the owner's choosing.
//
// A SearXNG of their own over plain HTTP, or Brave or Tavily with a key from the secret
// store, all bounded exactly as DuckDuckGo is: one query, an approved host, a rate
// limit, a response limit, and results written in the shape the quarantined reader
// splits. A provider that fails falls into the same DuckDuckGo and Wikipedia path with
// its reason kept.
namespace
{
using namespace std::chrono_literals;
using revia::actions::ActionRequest;
using revia::actions::ActionResult;
using revia::actions::ActionType;
using revia::actions::CapabilitySettings;
using revia::actions::PolicyDecision;
using revia::actions::internet::InternetSearchExecutor;
using revia::llm::HttpsRequest;
using revia::llm::HttpsResponse;
using revia::llm::HttpsTransport;
using revia::tests::Check;
using json = nlohmann::json;

struct Recorder
{
    std::mutex mutex;
    std::vector<HttpsRequest> requests;
    HttpsResponse reply;
};

class FakeTransport final : public HttpsTransport
{
public:
    explicit FakeTransport(std::shared_ptr<Recorder> inputRecorder) : recorder(std::move(inputRecorder)) {}
    HttpsResponse Post(const HttpsRequest& request, std::stop_token) override
    {
        std::lock_guard lock(recorder->mutex);
        recorder->requests.push_back(request);
        return recorder->reply;
    }
    std::string Describe() const override { return "fake transport"; }
    bool Available() const override { return true; }
private:
    std::shared_ptr<Recorder> recorder;
};

CapabilitySettings::InternetAccess Settings(const std::string& provider)
{
    CapabilitySettings::InternetAccess settings;
    settings.enabled = true;
    settings.automaticLookup = true;
    settings.visibleBrowser = false;
    settings.provider = provider;
    settings.maxResults = 5;
    settings.requestTimeoutMs = 3000;
    settings.providerKeyName = "revia-test-missing";
    settings.providerKeyEnvironmentVariable = "REVIA_TEST_UNSET_KEY_VARIABLE";
    return settings;
}

ActionRequest Search(const std::string& query)
{
    ActionRequest request;
    request.type = ActionType::WebSearch;
    request.value = query;
    request.requestedBy = "conversation_internet";
    return request;
}

void TestEachProvidersAnswerBecomesTheSharedShape()
{
    const ActionResult searxng = InternetSearchExecutor::ParseSearxngResponse(
        R"({"results":[{"title":"A page","url":"https://a.test/","content":"snippet a"},
            {"title":"Not web","url":"ftp://x.test/","content":"ignored"},
            {"title":"","url":"https://b.test/"}]})", 5);
    Check(searxng.succeeded && searxng.backend == "searxng" && searxng.entries.size() == 2 &&
        searxng.content.find("A page\nURL: https://a.test/\nsnippet a\nSource: https://a.test/") != std::string::npos &&
        searxng.content.find("Untitled result\nURL: https://b.test/\n(no summary)") != std::string::npos,
        "SearXNG results were not written in the reader's shape: " + searxng.content);
    const auto sources = revia::internet::SplitGroundingSources(searxng.content, searxng.entries);
    Check(sources.size() == 2 && sources[0].title == "A page" && sources[0].text == "snippet a" &&
        sources[1].url == "https://b.test/",
        "The reader could not split a provider's results.");
    const ActionResult brave = InternetSearchExecutor::ParseBraveResponse(
        R"({"web":{"results":[{"title":"B","url":"https://b.test/x","description":"described"}]}})", 5);
    Check(brave.succeeded && brave.backend == "brave_api" && brave.entries == std::vector<std::string>{"https://b.test/x"} &&
        brave.content.find("described") != std::string::npos && brave.message == "Brave Search returned 1 result.",
        "Brave's answer was not read.");
    const ActionResult tavily = InternetSearchExecutor::ParseTavilyResponse(
        R"({"results":[{"title":"T","url":"https://t.test/","content":"c"},{"title":"U","url":"https://u.test/","content":"d"}]})", 1);
    Check(tavily.succeeded && tavily.backend == "tavily_api" && tavily.entries.size() == 1,
        "Tavily's answer was not bounded to maxResults.");
    Check(!InternetSearchExecutor::ParseSearxngResponse("nope", 5).succeeded &&
        !InternetSearchExecutor::ParseBraveResponse(R"({"web":{}})", 5).succeeded &&
        !InternetSearchExecutor::ParseTavilyResponse(R"({"results":[]})", 5).succeeded,
        "An empty or broken answer was accepted as results.");

    const HttpsRequest braveRequest = InternetSearchExecutor::BuildProviderRequest(Settings("brave"), "llama cpp", "k1");
    bool token = false;
    for (const auto& [name, value] : braveRequest.headers)
        if (name == "X-Subscription-Token" && value == "k1") token = true;
    Check(braveRequest.method == "GET" && braveRequest.host == "api.search.brave.com" &&
        braveRequest.path.rfind("/res/v1/web/search?q=llama%20cpp&count=5", 0) == 0 && token && braveRequest.body.empty(),
        "The Brave request was not a keyed GET: " + braveRequest.path);
    const HttpsRequest tavilyRequest = InternetSearchExecutor::BuildProviderRequest(Settings("tavily"), "llama cpp", "k2");
    bool bearer = false;
    for (const auto& [name, value] : tavilyRequest.headers)
        if (name == "Authorization" && value == "Bearer k2") bearer = true;
    const json tavilyBody = json::parse(tavilyRequest.body);
    Check(tavilyRequest.method == "POST" && tavilyRequest.host == "api.tavily.com" && tavilyRequest.path == "/search" &&
        bearer && tavilyBody["query"] == "llama cpp" && tavilyBody["max_results"] == 5,
        "The Tavily request was not a keyed POST.");
    Check(InternetSearchExecutor::ProviderHost(Settings("searxng")) == "127.0.0.1" &&
        InternetSearchExecutor::ProviderHost(Settings("duckduckgo")) == "api.duckduckgo.com" &&
        InternetSearchExecutor::ProviderHost(Settings("nonsense")).empty(),
        "Providers did not map to their hosts.");
}

// A SearXNG of the owner's own, on this machine.
class FakeSearxng
{
public:
    FakeSearxng()
    {
        server.Get("/search", [this](const auto& request, auto& response)
        {
            {
                std::lock_guard lock(mutex);
                query = request.get_param_value("q");
                format = request.get_param_value("format");
            }
            response.set_content(json{{"results", json::array({
                {{"title", "Releases"}, {"url", "https://example.test/releases"}, {"content", "b9999 is out"}},
                {{"title", "Docs"}, {"url", "https://example.test/docs"}, {"content", "how to build"}}})}}.dump(),
                "application/json");
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind the SearXNG fixture.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        Check(server.is_running(), "The SearXNG fixture did not start.");
    }
    ~FakeSearxng() { server.stop(); thread.join(); }
    std::string Query() { std::lock_guard lock(mutex); return query; }
    std::string Format() { std::lock_guard lock(mutex); return format; }
    int port = 0;
private:
    httplib::Server server;
    std::mutex mutex;
    std::string query;
    std::string format;
    std::jthread thread;
};

void TestASearxngOfTheOwnersOwnIsSearchedOverPlainHttp()
{
    FakeSearxng searxng;
    CapabilitySettings::InternetAccess settings = Settings("searxng");
    settings.searxngHost = "127.0.0.1";
    settings.searxngPort = searxng.port;
    settings.approvedHosts = {"127.0.0.1"};
    InternetSearchExecutor executor(settings);
    const ActionResult result = executor.Execute(Search("llama cpp release"), PolicyDecision{});
    Check(result.succeeded && result.backend == "searxng" && result.entries.size() == 2 &&
        result.content.find("Releases\nURL: https://example.test/releases\nb9999 is out") != std::string::npos,
        "The SearXNG lookup did not return its results: " + result.message);
    Check(searxng.Query() == "llama cpp release" && searxng.Format() == "json",
        "SearXNG was not asked for JSON with the query as given.");
    settings.approvedHosts = {"api.duckduckgo.com"};
    InternetSearchExecutor unapproved(settings);
    const ActionResult refused = unapproved.Execute(Search("anything"), PolicyDecision{});
    Check(refused.message.find("SearXNG lookup failed (127.0.0.1 is not in the approved host list)") != std::string::npos,
        "A SearXNG host off the approved list was searched, or the fallback lost the reason: " + refused.message);
}

void TestAKeyedProviderLeavesThroughTheTransportAndFallsBackWithItsReason()
{
    auto recorder = std::make_shared<Recorder>();
    recorder->reply.completed = true;
    recorder->reply.status = 200;
    recorder->reply.body = json{{"web", {{"results", json::array({
        {{"title", "Brave hit"}, {"url", "https://brave.test/hit"}, {"description", "found"}}})}}}}.dump();
    CapabilitySettings::InternetAccess settings = Settings("brave");
    settings.approvedHosts = {"api.search.brave.com"};
    InternetSearchExecutor executor(settings);
    executor.SetHttpsTransport(std::make_unique<FakeTransport>(recorder));
    executor.SetProviderKey("brave-key");
    const ActionResult result = executor.Execute(Search("what is new"), PolicyDecision{});
    Check(result.succeeded && result.backend == "brave_api" && result.entries == std::vector<std::string>{"https://brave.test/hit"},
        "The Brave lookup did not return its result: " + result.message);
    {
        std::lock_guard lock(recorder->mutex);
        Check(recorder->requests.size() == 1 && recorder->requests.front().host == "api.search.brave.com",
            "The Brave request did not leave through the transport.");
        bool keyed = false;
        for (const auto& [name, value] : recorder->requests.front().headers)
            if (name == "X-Subscription-Token" && value == "brave-key") keyed = true;
        Check(keyed, "The key did not reach the request.");
    }
    recorder->reply.status = 401;
    recorder->reply.body = "{}";
    const ActionResult rejected = executor.Execute(Search("again"), PolicyDecision{});
    Check(!rejected.succeeded &&
        rejected.message.find("Brave Search API lookup failed (HTTP 401 (the key was rejected)). Bounded API fallback:") != std::string::npos,
        "A rejected key did not fall back with its reason: " + rejected.message);

    InternetSearchExecutor keyless(settings);
    keyless.SetHttpsTransport(std::make_unique<FakeTransport>(recorder));
    const ActionResult noKey = keyless.Execute(Search("no key"), PolicyDecision{});
    std::size_t sent = 0;
    {
        std::lock_guard lock(recorder->mutex);
        sent = recorder->requests.size();
    }
    Check(!noKey.succeeded && noKey.message.find("no key for brave") != std::string::npos &&
        noKey.message.find("SetAdvisorKey.ps1 -Name revia-test-missing") != std::string::npos && sent == 2,
        "Without a key the provider was still asked, or the way to store one was not named: " + noKey.message);
}
} // namespace

void RunSearchProviderTests()
{
    TestEachProvidersAnswerBecomesTheSharedShape();
    TestASearxngOfTheOwnersOwnIsSearchedOverPlainHttp();
    TestAKeyedProviderLeavesThroughTheTransportAndFallsBackWithItsReason();
    std::cout << "A SearXNG, Brave or Tavily lookup is bounded like DuckDuckGo's, written in the "
                 "reader's shape, keyed from the store, and falls back with its reason.\n";
}
