#include "Agents/reasoningRelay.h"

namespace revia::agents
{

void ReasoningRelay::Add(const std::string& delta)
{
    std::lock_guard lock(mutex);
    pending += delta;
    received += delta.size();
    for (;;)
    {
        std::size_t cut = std::string::npos;
        const std::size_t paragraph = pending.find("\n\n", MinimumCharacters);
        if (paragraph != std::string::npos && paragraph < MaximumCharacters)
        {
            cut = paragraph + 2;
        }
        else if (pending.size() >= MaximumCharacters)
        {
            cut = pending.rfind(' ', MaximumCharacters);
            if (cut == std::string::npos || cut < MinimumCharacters)
            {
                // No word boundary: cut anyway, but never inside a UTF-8 character.
                cut = MaximumCharacters;
                while (cut > 0 && (static_cast<unsigned char>(pending[cut]) & 0xC0U) == 0x80U)
                    --cut;
            }
        }
        if (cut == std::string::npos || cut == 0)
            break;
        Release(pending.substr(0, cut));
        pending.erase(0, cut);
    }
}

void ReasoningRelay::Flush()
{
    std::lock_guard lock(mutex);
    Release(pending);
    pending.clear();
}

std::size_t ReasoningRelay::ReceivedCharacters() const
{
    std::lock_guard lock(mutex);
    return received;
}

void ReasoningRelay::Release(const std::string& text) const
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || !publish)
        return;
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    publish(text.substr(first, last - first + 1));
}

} // namespace revia::agents
