#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace revia::agents
{

// Releases a reply while the model is still writing it, one complete sentence at a time,
// and only as far as the whole deterministic filter sequence leaves the reply so far
// unchanged. Raw model text never passes: a sentence the filters would edit, or whose
// tail might begin a prompt marker, waits for more text; a reply they refuse stops
// releasing, and the caller delivers the filtered final reply instead.
class ReplyStreamGate
{
public:
    // The filter sequence applied to a reply so far: the text it would deliver, or
    // nothing when it refuses the reply outright.
    using Filter = std::function<std::optional<std::string>(const std::string&)>;

    explicit ReplyStreamGate(Filter filter);

    // Feeds raw model text. Returns newly approved text, which extends Released().
    [[nodiscard]] std::string Consume(const std::string& delta);
    // Everything approved so far, exactly as it was released.
    [[nodiscard]] const std::string& Released() const;
    // The filters refused the reply so far; nothing more will be released.
    [[nodiscard]] bool Halted() const;

private:
    Filter filter;
    std::string raw;
    std::string released;
    // Sentence ends already judged. A prefix is judged once: the filters are a function
    // of the text alone, so asking again about the same prefix cannot change the answer.
    std::size_t judgedThrough = 0;
    bool halted = false;
};

} // namespace revia::agents
