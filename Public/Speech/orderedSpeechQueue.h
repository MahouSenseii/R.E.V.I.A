#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>

namespace revia::speech
{

// A tiny deterministic gate between parallel synthesis and serial playback. Workers may
// finish in any order; only the sequence at the front can leave, and only after that exact
// sequence is marked ready. SpeechService owns the mutex around this object.
class OrderedSpeechQueue
{
public:
    void Reserve(const std::uint64_t sequence)
    {
        order.push_back(sequence);
    }

    void MarkReady(const std::uint64_t sequence)
    {
        ready.insert(sequence);
    }

    [[nodiscard]] std::optional<std::uint64_t> FrontReady() const
    {
        if (order.empty() || !ready.contains(order.front())) return std::nullopt;
        return order.front();
    }

    [[nodiscard]] std::optional<std::uint64_t> PopFrontReady()
    {
        const std::optional<std::uint64_t> sequence = FrontReady();
        if (!sequence.has_value()) return std::nullopt;
        order.pop_front();
        ready.erase(*sequence);
        return sequence;
    }

    void Remove(const std::uint64_t sequence)
    {
        order.erase(std::remove(order.begin(), order.end(), sequence), order.end());
        ready.erase(sequence);
    }

    void Clear()
    {
        order.clear();
        ready.clear();
    }

    [[nodiscard]] bool Empty() const { return order.empty(); }

private:
    std::deque<std::uint64_t> order;
    std::set<std::uint64_t> ready;
};

} // namespace revia::speech
