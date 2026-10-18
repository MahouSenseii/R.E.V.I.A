#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revia::llm::cloud
{

// Where a cloud request goes, taken apart once so every later check reads fields
// instead of re-parsing a string.
struct CloudEndpoint
{
    bool secure = true;
    std::string host;
    int port = 443;
    // Path prefix from the base URL, without a trailing slash ("" or "/v1").
    std::string basePath;
};

// Accepts https://host[:port][/path]. Plain http is accepted only for 127.0.0.1, which
// is what a local test server listens on; anything else would send the conversation
// and the key unencrypted. Returns nullopt with outError set otherwise.
[[nodiscard]] std::optional<CloudEndpoint> ParseCloudEndpoint(const std::string& baseUrl, std::string& outError);

struct CloudHttpRequest
{
    CloudEndpoint endpoint;
    // Full request path, for example "/v1/chat/completions".
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeoutSeconds = 60;
};

struct CloudHttpResult
{
    // True when a response arrived, whatever its status.
    bool bReceived = false;
    int status = 0;
    std::string error;
    std::size_t bytesReceived = 0;
};

// One POST whose response body is handed over as it arrives, so server-sent events can
// be decoded while the model is still writing. Each chunk comes with the HTTP status, so
// an error body is never shown as reply text. Returning false from onChunk stops the
// transfer. Tests replace the platform transport with a fake that never touches the
// network.
class CloudTransport
{
public:
    using ChunkHandler = std::function<bool(int status, std::string_view chunk)>;
    virtual ~CloudTransport() = default;
    [[nodiscard]] virtual CloudHttpResult Post(const CloudHttpRequest& request, const ChunkHandler& onChunk,
        std::stop_token stopToken) = 0;
};

// WinHTTP on Windows. Elsewhere a transport that refuses every request: cpp-httplib is
// built without OpenSSL here, and an unencrypted fallback is exactly what must not exist.
[[nodiscard]] std::shared_ptr<CloudTransport> MakePlatformCloudTransport();

} // namespace revia::llm::cloud
