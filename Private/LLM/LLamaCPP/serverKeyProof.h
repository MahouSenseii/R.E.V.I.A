#pragma once

#include "cancellableHttpClient.h"

#include <httplib.h>
#include <stop_token>
#include <string>

namespace revia::llm
{
// Whether the server at host:port was started with `apiKey`. It must refuse a request
// carrying a wrong key and accept the same request carrying apiKey. A healthy answer on
// the port proves nothing: any local process can listen there, and a server started
// without a key accepts every key, so either would otherwise be sent every private
// prompt. `path` is an endpoint the server always protects with its key; the body is an
// empty object, so a request that passes the key check fails validation instead of
// running inference.
inline bool ProvesServerKey(const std::string& host, const int port, const std::string& apiKey, const std::string& path,
    const std::stop_token stopToken, std::string& outReason)
{
    const auto post = [&](const std::string& key)
    {
        CancellableHttpClient client(host, port, stopToken);
        client.set_connection_timeout(2);
        client.set_read_timeout(5);
        std::stop_callback cancel(stopToken, [&client] { client.stop(); });
        const auto result = client.Post(path, httplib::Headers{{"Authorization", "Bearer " + key}}, "{}", "application/json");
        return result ? result->status : -1;
    };
    const std::string where = host + ":" + std::to_string(port);
    if (apiKey.empty())
    {
        outReason = "No key was configured to check the server at " + where + " against.";
        return false;
    }
    const int wrong = post(apiKey + "-not-this-session");
    if (wrong < 0)
    {
        outReason = "The server at " + where + " did not answer the key check.";
        return false;
    }
    if (wrong != 401 && wrong != 403)
    {
        outReason = "The server at " + where + " accepts a wrong key, so it is not the one Revia started with this session's key.";
        return false;
    }
    const int right = post(apiKey);
    if (right < 0 || right == 401 || right == 403)
    {
        outReason = "The server at " + where + " refused this session's key (" +
                    (right < 0 ? std::string("no response") : "HTTP " + std::to_string(right)) +
                    "), so it was started by something other than Revia.";
        return false;
    }
    return true;
}
}
