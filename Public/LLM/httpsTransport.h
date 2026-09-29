#pragma once

#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace revia::llm
{

// One HTTPS POST, for the requests that leave this machine.
//
// The local workers are spoken to over plain HTTP by the httplib client, which is built
// without TLS. A hosted model is not reachable that way, and bundling a TLS stack for
// it would mean shipping and updating one. Windows already has one: WinHTTP, with the
// system's certificate validation and proxy settings. So the transport is the OS's on
// Windows and a refusal elsewhere, behind an interface the tests replace with a fake.
struct HttpsRequest
{
    std::string host;
    int port = 443;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeoutSeconds = 60;
};

struct HttpsResponse
{
    // False when no response was read at all; `error` then says why.
    bool completed = false;
    int status = 0;
    std::string body;
    std::string error;
};

class HttpsTransport
{
public:
    virtual ~HttpsTransport() = default;
    [[nodiscard]] virtual HttpsResponse Post(const HttpsRequest& request, std::stop_token stopToken) = 0;
    // What the status line calls it.
    [[nodiscard]] virtual std::string Describe() const = 0;
    // Whether this transport can send at all.
    [[nodiscard]] virtual bool Available() const = 0;
};

[[nodiscard]] std::unique_ptr<HttpsTransport> MakeSystemHttpsTransport();

} // namespace revia::llm
