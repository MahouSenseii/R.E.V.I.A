#pragma once

#include "Runtime/runtimeStamp.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace revia::browser
{

struct BrowserSettings
{
    bool enabled = false;
    bool navigate = false;
    bool interact = false;
    bool allowTaskInteraction = false;
    bool allowLoopback = false;
    std::vector<std::string> approvedOrigins;
    int timeoutMs = 15000;
    std::size_t maxTextBytes = 8192;
    std::size_t maxElements = 60;
    std::size_t maxValueBytes = 4096;
};

struct BrowserRequest
{
    std::string url;
    std::string session;
    std::uint64_t generation = 0;
    std::string element;
    std::string value;
};

struct BrowserElement
{
    std::string id;
    std::string name;
    std::string role;
    bool clickable = false;
    bool editable = false;
    std::string value;
    bool valueAvailable = false;
};

struct BrowserReceipt
{
    std::string session;
    std::uint64_t generation = 0;
    std::string url;
    std::string title;
    std::string text;
    std::string fingerprint;
    std::vector<BrowserElement> elements;
    bool uncertainEffect = false;
    runtime::RuntimeStamp authorityStamp;
};

[[nodiscard]] BrowserSettings IntersectSettings(const BrowserSettings& first, const BrowserSettings& second);
[[nodiscard]] bool IsApprovedUrl(const std::string& url, const BrowserSettings& settings);
[[nodiscard]] bool ValidateSettings(const BrowserSettings& settings, std::string& error);
[[nodiscard]] std::string UrlOrigin(const std::string& url);

} // namespace revia::browser
