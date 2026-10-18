#pragma once

#include "Policy/guestCapabilityPolicy.h"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace revia::policy
{
// The permission file never selects an environment. Only the trusted goal store
// persists the identity captured by admission, under the explicit storedGoal flag.
[[nodiscard]] inline nlohmann::json GuestCapabilitiesToJson(const GuestCapabilitySettings& settings, const bool storedGoal = false)
{
    const auto& limits = settings.limits;
    nlohmann::json result = {{"enabled", settings.enabled}, {"read", settings.read}, {"write", settings.write},
        {"execute", settings.execute}, {"allowTaskExecution", settings.allowTaskExecution},
        {"approvedExecutables", limits.approvedExecutables}, {"maxReadBytes", limits.maxReadBytes},
        {"maxWriteBytes", limits.maxWriteBytes}, {"maxOutputBytes", limits.maxOutputBytes},
        {"maxEntries", limits.maxEntries}, {"maxTimeoutMs", limits.maxTimeoutMs}};
    if (storedGoal)
        result["expectedIdentity"] = {{"environmentId", settings.expectedIdentity.environmentId}, {"bootId", settings.expectedIdentity.bootId}};
    return result;
}

[[nodiscard]] inline GuestCapabilitySettings GuestCapabilitiesFromJson(const nlohmann::json& data, const bool storedGoal = false)
{
    if (!data.is_object()) throw std::invalid_argument("guest must be a capability object.");
    for (const auto& entry : data.items())
    {
        const auto& key = entry.key();
        if (key != "enabled" && key != "read" && key != "write" && key != "execute" && key != "allowTaskExecution" &&
            key != "approvedExecutables" && key != "maxReadBytes" && key != "maxWriteBytes" && key != "maxOutputBytes" &&
            key != "maxEntries" && key != "maxTimeoutMs" && !(storedGoal && key == "expectedIdentity"))
            throw std::invalid_argument("Unknown guest capability field.");
    }
    GuestCapabilitySettings result;
    result.enabled = data.value("enabled", false);
    result.read = data.value("read", false);
    result.write = data.value("write", false);
    result.execute = data.value("execute", false);
    result.allowTaskExecution = data.value("allowTaskExecution", false);
    const auto number = [&](const char* key, const std::int64_t fallback, const std::int64_t maximum)
    {
        if (!data.contains(key)) return fallback;
        if (!data.at(key).is_number_integer()) throw std::invalid_argument("Guest limits must be integers.");
        const auto value = data.at(key).get<std::int64_t>();
        if (value < 1 || value > maximum) throw std::invalid_argument("Guest limits exceed the supported range.");
        return value;
    };
    result.limits.maxReadBytes = static_cast<std::size_t>(number("maxReadBytes", 65536, 262144));
    result.limits.maxWriteBytes = static_cast<std::size_t>(number("maxWriteBytes", 65536, 262144));
    result.limits.maxOutputBytes = static_cast<std::size_t>(number("maxOutputBytes", 65536, 65536));
    result.limits.maxEntries = static_cast<std::size_t>(number("maxEntries", 256, 512));
    result.limits.maxTimeoutMs = static_cast<int>(number("maxTimeoutMs", 120000, 120000));
    result.limits.approvedExecutables = data.value("approvedExecutables", std::vector<std::string>{});
    if (result.limits.approvedExecutables.size() > 128) throw std::invalid_argument("Guest executable allowlist exceeds its limit.");
    for (const auto& executable : result.limits.approvedExecutables)
    {
        auto validation = result;
        validation.enabled = validation.execute = true;
        workspace::GuestRequest request;
        request.operation = workspace::GuestOperation::ExecuteProcess;
        request.executable = executable;
        request.workingDirectory = ".";
        request.timeoutMs = 1;
        if (!EvaluateGuestCapability(request, validation).allowed)
            throw std::invalid_argument("Guest executables must be normalized absolute POSIX paths.");
    }
    if (storedGoal && data.contains("expectedIdentity"))
    {
        const auto& identity = data.at("expectedIdentity");
        if (!identity.is_object()) throw std::invalid_argument("Stored guest identity must be an object.");
        result.expectedIdentity.environmentId = identity.value("environmentId", std::string{});
        result.expectedIdentity.bootId = identity.value("bootId", std::string{});
        if (result.expectedIdentity.environmentId.size() > 128 || result.expectedIdentity.bootId.size() > 128)
            throw std::invalid_argument("Stored guest identity exceeds its limit.");
    }
    return result;
}
}
