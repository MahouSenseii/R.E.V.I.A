#include "Browser/browserTypes.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace revia::browser
{
namespace
{

std::string Origin(const std::string& url, const bool allowLoopback)
{
    if (url.size() > 4096 || url.find_first_of("\r\n\t \\") != std::string::npos || url.find('\0') != std::string::npos)
        return {};
    static const std::regex pattern(R"(^(https?)://([a-zA-Z0-9.-]+|\[::1\])(?::([0-9]{1,5}))?([/?#].*)?$)");
    std::smatch match;
    if (!std::regex_match(url, match, pattern))
        return {};
    std::string host = match[2].str();
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (host.empty() || host.back() == '.')
        return {};
    const bool loopback = host == "localhost" || host == "127.0.0.1" || host == "[::1]";
    if (loopback && !allowLoopback)
        return {};
    if (!loopback && match[1] != "https")
        return {};
    if (!loopback && std::all_of(host.begin(), host.end(), [](unsigned char c) { return std::isdigit(c) || c == '.'; }))
        return {};
    std::string port = match[3].str();
    if (!port.empty())
    {
        const auto number = std::stoi(port);
        if (number < 1 || number > 65535 || port != std::to_string(number))
            return {};
        if ((match[1] == "https" && number == 443) || (match[1] == "http" && number == 80))
            port.clear();
    }
    return match[1].str() + "://" + host + (port.empty() ? "" : ":" + port);
}

} // namespace

std::string UrlOrigin(const std::string& url)
{
    return Origin(url, true);
}

bool IsApprovedUrl(const std::string& url, const BrowserSettings& settings)
{
    const auto origin = Origin(url, settings.allowLoopback);
    return settings.enabled && !origin.empty() &&
           std::find(settings.approvedOrigins.begin(), settings.approvedOrigins.end(), origin) != settings.approvedOrigins.end();
}

bool ValidateSettings(const BrowserSettings& settings, std::string& error)
{
    if (settings.timeoutMs < 100 || settings.timeoutMs > 60000 || settings.maxTextBytes < 256 || settings.maxTextBytes > 65536 ||
        settings.maxElements < 1 || settings.maxElements > 200 || settings.maxValueBytes < 1 || settings.maxValueBytes > 16384 ||
        settings.approvedOrigins.size() > 64)
    {
        error = "Browser limits are outside the bounded interactive contract.";
        return false;
    }
    for (const auto& origin : settings.approvedOrigins)
    {
        if (origin.empty() || Origin(origin, settings.allowLoopback) != origin)
        {
            error = "Browser origins must be exact canonical HTTPS origins; loopback HTTP requires its separate grant.";
            return false;
        }
    }
    error.clear();
    return true;
}

BrowserSettings IntersectSettings(const BrowserSettings& first, const BrowserSettings& second)
{
    BrowserSettings result;
    result.enabled = first.enabled && second.enabled;
    result.navigate = first.navigate && second.navigate;
    result.interact = first.interact && second.interact;
    result.allowTaskInteraction = first.allowTaskInteraction && second.allowTaskInteraction;
    result.allowLoopback = first.allowLoopback && second.allowLoopback;
    for (const auto& origin : first.approvedOrigins)
        if (std::find(second.approvedOrigins.begin(), second.approvedOrigins.end(), origin) != second.approvedOrigins.end())
            result.approvedOrigins.push_back(origin);
    result.timeoutMs = std::min(first.timeoutMs, second.timeoutMs);
    result.maxTextBytes = std::min(first.maxTextBytes, second.maxTextBytes);
    result.maxElements = std::min(first.maxElements, second.maxElements);
    result.maxValueBytes = std::min(first.maxValueBytes, second.maxValueBytes);
    return result;
}

} // namespace revia::browser
