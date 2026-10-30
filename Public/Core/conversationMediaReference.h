#pragma once

#include <cstdint>
#include <string>

enum class MediaOrigin
{
    UserAttachment,
    GeneratedArtifact
};

// An immutable association with stored media. Access still requires the current
// conversation's scope and a validated attachment-store lookup.
struct conversationMediaReference
{
    std::string id;
    std::string digest;
    std::string name;
    std::string mimeType;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int64_t durationMs = 0;
    std::int64_t sampleStartMs = 0;
    std::int64_t sampleEndMs = 0;
    MediaOrigin origin = MediaOrigin::UserAttachment;

    bool operator==(const conversationMediaReference&) const = default;
};
