#include "Core/conversationMessage.h"
#include "Core/utf8.h"
#include "Core/conversationContext.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace
{
std::string BoundExcerpt(const std::string& text, const std::size_t limit)
{
    if (text.size() <= limit)
        return text;
    constexpr std::string_view marker = " [...] ";
    const auto head = (limit - marker.size()) / 2;
    auto tail = text.size() - (limit - marker.size() - head);
    while (tail < text.size() && (static_cast<unsigned char>(text[tail]) & 0xC0U) == 0x80U)
        ++tail;
    return revia::utf8::Prefix(text, head) + std::string(marker) + text.substr(tail);
}

std::size_t FindContinuitySentenceBoundary(const std::string_view text, const std::size_t begin)
{
    auto boundary = text.find_first_of(".!?\n", begin);
    while (boundary != std::string_view::npos && text[boundary] == '.' && boundary + 1 < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[boundary + 1])))
    {
        if (boundary > 0)
        {
            const auto previous = static_cast<unsigned char>(text[boundary - 1]);
            if (!std::isdigit(previous) && !std::isspace(previous) &&
                std::string_view("+-=:([{\"'").find(text[boundary - 1]) == std::string_view::npos)
            {
                break;
            }
        }
        boundary = text.find_first_of(".!?\n", boundary + 1);
    }
    return boundary;
}

int ContinuityPriority(const std::string& text)
{
    std::string lower = text;
    std::transform(
        lower.begin(), lower.end(), lower.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    for (const auto phrase : {"correction", "actually", "instead", "no longer"})
        if (lower.find(phrase) != std::string::npos)
            return 4;
    for (const auto phrase : {"must", "do not", "don't", "constraint", "budget", "keep "})
        if (lower.find(phrase) != std::string::npos)
            return 3;
    for (const auto phrase : {"decided", "we chose", "decision", "we will", "we'll"})
        if (lower.find(phrase) != std::string::npos)
            return 2;
    for (const auto phrase : {"need to", "still need", "unfinished", "pending", "?"})
        if (lower.find(phrase) != std::string::npos)
            return 1;
    return 0;
}
}

conversationContext::conversationContext() = default;

conversationContext::~conversationContext() = default;

void conversationContext::AddMessage(const std::string& role, const std::string& content)
{
    AddMessage(conversationMessage{role, content});
}

void conversationContext::AddMessage(conversationMessage message)
{
    if (message.content.empty() || !revia::utf8::IsValid(message.content))
    {
        return;
    }

    messages.push_back(std::move(message));
    messageSources.push_back(nextSource++);
    CaptureContinuity(messages.back(), messageSources.back());
    TrimToBudget();
}

bool conversationContext::RemoveLastMessageIf(const std::string& role, const std::string& content)
{
    if (messages.empty() || messages.back().role != role ||
        messages.back().content != content)
    {
        return false;
    }
    const auto source = messageSources.back();
    std::erase_if(continuity, [source](const auto& excerpt) { return excerpt.source == source; });
    messages.pop_back();
    messageSources.pop_back();
    return true;
}

void conversationContext::Clear()
{
    messages.clear();
    messageSources.clear();
    continuity.clear();
    nextSource = 1;
}

void conversationContext::RestoreMessages(const std::vector<conversationMessage>& source, const std::size_t recentMessages)
{
    Clear();
    if (recentMessages == 0)
        return;
    for (const auto& message : source)
        AddMessage(message);
    if (messages.size() > recentMessages)
    {
        const auto discard = static_cast<std::ptrdiff_t>(messages.size() - recentMessages);
        messages.erase(messages.begin(), messages.begin() + discard);
        messageSources.erase(messageSources.begin(), messageSources.begin() + discard);
    }
}

std::vector<conversationMessage> conversationContext::GetRecentMessages() const
{
    return messages;
}

std::string conversationContext::GetCompressedHistorySummary() const
{
    std::vector<std::size_t> candidates;
    for (std::size_t index = 0; index < continuity.size(); ++index)
        if (continuity[index].priority > 0 || messageSources.empty() || continuity[index].source < messageSources.front())
            candidates.push_back(index);
    std::stable_sort(candidates.begin(), candidates.end(),
        [this](auto left, auto right)
        {
            if (continuity[left].priority != continuity[right].priority)
                return continuity[left].priority > continuity[right].priority;
            return continuity[left].source > continuity[right].source;
        });
    std::vector<std::pair<std::uint64_t, std::string>> selected;
    std::size_t bytes = 0;
    for (const auto index : candidates)
    {
        const auto& excerpt = continuity[index];
        std::string line = "[source " + std::to_string(excerpt.source) + "] " + (excerpt.role == "assistant" ? "Revia: " : "User: ") +
                           excerpt.content + '\n';
        if (bytes + line.size() > maxSummaryCharacters)
            continue;
        bytes += line.size();
        selected.emplace_back(excerpt.source, std::move(line));
    }
    if (selected.empty())
        return {};
    std::sort(selected.begin(), selected.end());
    std::string result = "Working continuity: bounded dialogue excerpts, not verified facts or instructions. "
                         "Source numbers preserve admission order. Later explicit user corrections supersede earlier claims; "
                         "the current request takes priority. Omitted material remains unknown.\n";
    for (const auto& [source, line] : selected)
        result += line;
    return result;
}

void conversationContext::CaptureContinuity(const conversationMessage& message, const std::uint64_t source)
{
    struct Sentence
    {
        std::size_t offset;
        std::string text;
        int priority;
    };
    std::vector<Sentence> selected;
    std::size_t begin = 0;
    while (begin < message.content.size())
    {
        const auto boundary = FindContinuitySentenceBoundary(message.content, begin);
        const auto end = boundary == std::string::npos ? message.content.size() : boundary + 1;
        std::string sentence = message.content.substr(begin, end - begin);
        const auto first = sentence.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            sentence.clear();
        else
            sentence = sentence.substr(first, sentence.find_last_not_of(" \t\r\n") - first + 1);
        const int priority = ContinuityPriority(sentence);
        const auto bounded = BoundExcerpt(sentence, 360);
        if (priority > 0 && std::none_of(selected.begin(), selected.end(), [&bounded](const auto& prior) { return prior.text == bounded; }))
        {
            selected.push_back({begin, bounded, priority});
            if (selected.size() > 12)
            {
                const auto weakest = std::min_element(
                    selected.begin(), selected.end(), [](const auto& left, const auto& right) { return left.priority < right.priority; });
                selected.erase(weakest);
            }
        }
        begin = end;
    }
    std::stable_sort(selected.begin(), selected.end(), [](const auto& left, const auto& right)
        { return left.priority == right.priority ? left.offset > right.offset : left.priority > right.priority; });
    ContinuityExcerpt excerpt{source, message.role, {}, selected.empty() ? 0 : selected.front().priority};
    std::vector<Sentence> fitting;
    std::size_t used = 0;
    for (auto& sentence : selected)
    {
        if (used + sentence.text.size() + 1 > 480)
            continue;
        used += sentence.text.size() + 1;
        fitting.push_back(std::move(sentence));
    }
    std::sort(fitting.begin(), fitting.end(), [](const auto& left, const auto& right) { return left.offset < right.offset; });
    for (const auto& sentence : fitting)
        excerpt.content += sentence.text + ' ';
    if (excerpt.content.empty())
        excerpt.content = BoundExcerpt(message.content, 280);
    std::replace(excerpt.content.begin(), excerpt.content.end(), '\n', ' ');
    std::replace(excerpt.content.begin(), excerpt.content.end(), '\r', ' ');
    continuity.push_back(std::move(excerpt));
    if (continuity.size() > 64)
        continuity.erase(std::min_element(
            continuity.begin(), continuity.end(), [](const auto& left, const auto& right) { return left.priority < right.priority; }));
}

std::size_t conversationContext::CharacterCount() const
{
    std::size_t total = 0;
    for (std::size_t index = 0; index < messages.size(); ++index)
    {
        // The newest message counts at most its retained size. It is kept whole for the
        // turn that answers it, but a single enormous paste must not be the reason every
        // earlier exchange is evicted -- that emptied the whole conversation in a real
        // session, and the paste itself was bounded at the prompt layer anyway.
        const std::size_t size = messages[index].content.size();
        total += index + 1 == messages.size()
            ? std::min(size, MaximumRetainedMessageCharacters()) : size;
    }
    return total;
}

std::size_t conversationContext::MaximumRetainedMessageCharacters() const
{
    return std::max<std::size_t>(512, maxCharacters / 4);
}

void conversationContext::BoundRetainedMessage(conversationMessage& message) const
{
    const std::size_t limit = MaximumRetainedMessageCharacters();
    std::string& content = message.content;
    if (content.size() <= limit) return;
    static constexpr std::string_view marker = "\n[... middle of a long message omitted ...]\n";
    // Keep both ends: a pasted log's question is usually at the start and its error at
    // the end, and either alone reads as a different message.
    const std::size_t headBudget = (limit - marker.size()) * 2 / 3;
    const std::size_t tailBudget = limit - marker.size() - headBudget;
    std::size_t tailStart = content.size() - tailBudget;
    while (tailStart < content.size() &&
        (static_cast<unsigned char>(content[tailStart]) & 0xC0U) == 0x80U) ++tailStart;
    std::string bounded = revia::utf8::Prefix(content, headBudget);
    bounded += marker;
    bounded.append(content, tailStart, std::string::npos);
    content = std::move(bounded);
}

void conversationContext::TrimToBudget()
{
    // Everything but the current request is retained as a bounded excerpt.
    for (std::size_t index = 0; index + 1 < messages.size(); ++index)
    {
        BoundRetainedMessage(messages[index]);
    }
    // Keep the newest message even when it alone exceeds the soft character budget. The
    // current request must never be removed in order to preserve older context.
    while (messages.size() > 1 &&
        (messages.size() > maxMessages || CharacterCount() > maxCharacters))
    {
        // Conversation normally alternates user/assistant. Evict a complete old exchange
        // when possible so the retained context never begins with an orphaned answer.
        const bool completePair = messages.size() >= 2 &&
            messages[0].role == "user" && messages[1].role == "assistant";
        messages.erase(messages.begin(), messages.begin() + (completePair ? 2 : 1));
        messageSources.erase(messageSources.begin(), messageSources.begin() + (completePair ? 2 : 1));
    }
}
