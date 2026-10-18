#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>

namespace revia::agents
{

// Releases a model's streamed thinking in readable pieces.
//
// Thinking arrives a few characters at a time, and every piece the window is given
// becomes its own "Revia is thinking" block. Passing each delta through would bury the
// chat in fragments; holding everything to the end would show nothing while she thinks.
// So text is released at a paragraph break once a piece is long enough, or at a word
// boundary once it is too long to wait for one, and whatever is left on Flush.
class ReasoningRelay
{
public:
    using Publisher = std::function<void(const std::string&)>;
    static constexpr std::size_t MinimumCharacters = 200;
    static constexpr std::size_t MaximumCharacters = 1200;

    explicit ReasoningRelay(Publisher publisher) : publish(std::move(publisher)) {}

    void Add(const std::string& delta);
    // Releases what is still held. Called once thinking has ended.
    void Flush();
    // Everything received so far, released or not.
    [[nodiscard]] std::size_t ReceivedCharacters() const;

private:
    void Release(const std::string& text) const;

    Publisher publish;
    mutable std::mutex mutex;
    std::string pending;
    std::size_t received = 0;
};

} // namespace revia::agents
