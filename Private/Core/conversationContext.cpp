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

bool NamesFileToken(const std::string_view prefix)
{
    std::string lower(prefix);
    std::transform(
        lower.begin(), lower.end(), lower.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    for (const auto cue : {"save ", "filename ", "file name ", "file ", "path "})
    {
        const auto position = lower.find(cue);
        if (position != std::string::npos && (position == 0 || std::isspace(static_cast<unsigned char>(lower[position - 1]))))
            return true;
    }
    return false;
}

std::size_t FindContinuitySentenceBoundary(const std::string_view text, const std::size_t begin)
{
    auto boundary = text.find_first_of(".!?\n", begin);
    while (boundary != std::string_view::npos && boundary + 1 < text.size())
    {
        const auto next = static_cast<unsigned char>(text[boundary + 1]);
        if (text[boundary] != '.')
        {
            const auto tokenStart = text.find_last_of(" \t\r\n", boundary);
            const auto token = text.substr(tokenStart == std::string_view::npos ? 0 : tokenStart + 1,
                boundary - (tokenStart == std::string_view::npos ? 0 : tokenStart + 1));
            if ((text[boundary] == '?' || text[boundary] == '!') && !std::isspace(next) && token.find("://") != std::string_view::npos)
            {
                boundary = text.find_first_of(".!?\n", boundary + 1);
                continue;
            }
            break;
        }
        const bool decimal = std::isdigit(next) != 0;
        const auto tokenStart = text.find_last_of(" \t\r\n", boundary);
        const auto tokenBegin = tokenStart == std::string_view::npos ? 0 : tokenStart + 1;
        const auto tokenEnd = text.find_first_of(" \t\r\n", boundary + 1);
        const auto token = text.substr(tokenBegin, (tokenEnd == std::string_view::npos ? text.size() : tokenEnd) - tokenBegin);
        auto digitsEnd = boundary + 1;
        while (digitsEnd < text.size() && std::isdigit(static_cast<unsigned char>(text[digitsEnd])))
            ++digitsEnd;
        const bool numericLabel =
            decimal && digitsEnd < text.size() &&
            (std::isalpha(static_cast<unsigned char>(text[digitsEnd])) ||
                (text[digitsEnd] == '.' && digitsEnd + 1 < text.size() && std::isalpha(static_cast<unsigned char>(text[digitsEnd + 1]))));
        const bool pathToken = token.find_first_of("/\\") != std::string_view::npos;
        // Explicit filename context makes the token opaque; numeric suffixes alone
        // remain ambiguous with the existing decimal and sentence-boundary rules.
        const bool fileToken = tokenBegin >= begin && NamesFileToken(text.substr(begin, tokenBegin - begin));
        const bool dottedToken =
            !std::isspace(next) &&
            (pathToken || (boundary > tokenBegin && (fileToken || std::isalpha(next) != 0 || next == '_' || numericLabel)));
        if (!decimal && !dottedToken)
            break;
        if (decimal && !dottedToken && boundary > 0)
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
    // An answered question is not an outstanding task; ordinary questions remain available after history eviction.
    for (const auto phrase : {"need to", "still need", "unfinished", "pending"})
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
    CaptureMediaHistory(messages.back(), messageSources.back());
    CaptureContinuity(messages.back(), messageSources.back());
    TrimToBudget();
}

bool conversationContext::RemoveLastMessageIf(const std::string& role, const std::string& content)
{
    if (messages.empty() || messages.back().role != role || messages.back().content != content)
    {
        return false;
    }
    const auto source = messageSources.back();
    std::erase_if(continuity, [source](const auto& excerpt) { return excerpt.source == source; });
    std::erase_if(mediaHistory, [source](const auto& record) { return record.source == source; });
    messages.pop_back();
    messageSources.pop_back();
    return true;
}

void conversationContext::Clear()
{
    messages.clear();
    messageSources.clear();
    continuity.clear();
    mediaHistory.clear();
    nextSource = 1;
}

void conversationContext::InvalidateMediaAnalysis(const std::string& id)
{
    bool affected = false;
    for (auto& message : messages)
    {
        const bool generatedSource = message.role == "assistant" &&
                                     std::any_of(message.mediaReferences.begin(), message.mediaReferences.end(), [&](const auto& reference)
                                         { return reference.id == id && reference.origin == MediaOrigin::GeneratedArtifact; });
        if (message.role == "user")
            affected = std::any_of(
                message.mediaReferences.begin(), message.mediaReferences.end(), [&](const auto& reference) { return reference.id == id; });
        else if (message.role == "assistant" && (affected || generatedSource))
        {
            message.content = "[Earlier image analysis was cleared because its source image was forgotten.]";
            affected = generatedSource;
        }
        message.images.clear();
    }
    std::erase_if(continuity,
        [&](const auto& excerpt) { return std::find(excerpt.mediaIds.begin(), excerpt.mediaIds.end(), id) != excerpt.mediaIds.end(); });
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

std::vector<conversationMessage> conversationContext::GetMediaHistory() const
{
    std::vector<conversationMessage> history;
    history.reserve(mediaHistory.size());
    for (const auto& record : mediaHistory)
        history.push_back(record.message);
    return history;
}

void conversationContext::CaptureMediaHistory(const conversationMessage& message, const std::uint64_t source)
{
    const bool generated = message.role == "assistant" && message.memoryScope &&
                           revia::memory::IsAttributedPrivateScope(*message.memoryScope) &&
                           std::all_of(message.mediaReferences.begin(), message.mediaReferences.end(),
                               [](const auto& reference) { return reference.origin == MediaOrigin::GeneratedArtifact; });
    if ((message.role != "user" && !generated) || message.mediaReferences.empty())
        return;
    conversationMessage metadata;
    metadata.role = message.role;
    metadata.participantId = message.participantId;
    metadata.memoryScope = message.memoryScope;
    metadata.mediaReferences.assign(
        message.mediaReferences.begin(), message.mediaReferences.begin() + std::min<std::size_t>(4, message.mediaReferences.size()));
    mediaHistory.push_back({source, std::move(metadata)});
    if (mediaHistory.size() > 256)
        mediaHistory.erase(mediaHistory.begin());
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
    ContinuityExcerpt excerpt{source, message.role, {}, selected.empty() ? 0 : selected.front().priority, {}};
    if (message.role == "assistant")
        for (const auto& reference : message.mediaReferences)
            if (reference.origin == MediaOrigin::GeneratedArtifact)
                excerpt.mediaIds.push_back(reference.id);
    if (message.role == "assistant" && messages.size() >= 2 && messages[messages.size() - 2].role == "user")
        for (const auto& reference : messages[messages.size() - 2].mediaReferences)
            excerpt.mediaIds.push_back(reference.id);
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
        total += index + 1 == messages.size() ? std::min(size, MaximumRetainedMessageCharacters()) : size;
    }
    return total;
}

std::size_t conversationContext::MaximumRetainedMessageCharacters() const
{
    return std::max<std::size_t>(512, maxRetainedMessageCharacters);
}

void conversationContext::SetContextTokens(const int contextTokens)
{
    // Anchored at two points: the 8,192-token caps the conversation was tuned with, and
    // about 60 messages, 60,000 characters and 12,000 characters per older message at
    // 32,768. Linear between them and beyond, proportional below. These are soft bounds
    // on what is kept; the prompt is still fitted to the real context before each request.
    const double tokens = std::clamp(contextTokens, 2048, 131072);
    if (tokens <= 8192.0)
    {
        const double scale = tokens / 8192.0;
        maxMessages = std::max<std::size_t>(8, static_cast<std::size_t>(24.0 * scale));
        maxCharacters = std::max<std::size_t>(4000, static_cast<std::size_t>(14000.0 * scale));
        maxRetainedMessageCharacters = std::max<std::size_t>(1000, static_cast<std::size_t>(3500.0 * scale));
    }
    else
    {
        const double step = (tokens - 8192.0) / (32768.0 - 8192.0);
        maxMessages = std::min<std::size_t>(120, static_cast<std::size_t>(24.0 + 36.0 * step));
        maxCharacters = std::min<std::size_t>(120000, static_cast<std::size_t>(14000.0 + 46000.0 * step));
        maxRetainedMessageCharacters = std::min<std::size_t>(24000, static_cast<std::size_t>(3500.0 + 8500.0 * step));
    }
    TrimToBudget();
}

void conversationContext::BoundRetainedMessage(conversationMessage& message) const
{
    const std::size_t limit = MaximumRetainedMessageCharacters();
    std::string& content = message.content;
    if (content.size() <= limit)
        return;
    static constexpr std::string_view marker = "\n[... middle of a long message omitted ...]\n";
    // Keep both ends: a pasted log's question is usually at the start and its error at
    // the end, and either alone reads as a different message.
    const std::size_t headBudget = (limit - marker.size()) * 2 / 3;
    const std::size_t tailBudget = limit - marker.size() - headBudget;
    std::size_t tailStart = content.size() - tailBudget;
    while (tailStart < content.size() && (static_cast<unsigned char>(content[tailStart]) & 0xC0U) == 0x80U)
        ++tailStart;
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
    if (messages.size() <= 1 || (messages.size() <= maxMessages && CharacterCount() <= maxCharacters))
    {
        return;
    }
    // Over budget, trim well below it rather than to just under it. Dropping one exchange
    // every turn changed the start of the history on every request, so llama.cpp could
    // reuse almost none of its cached prompt. Down to 60%, the retained history stays
    // byte-identical for several turns before the next trim.
    const std::size_t messageTarget = std::max<std::size_t>(2, maxMessages * 3 / 5);
    const std::size_t characterTarget = maxCharacters * 3 / 5;
    // Keep the newest message even when it alone exceeds the soft character budget. The
    // current request must never be removed in order to preserve older context.
    while (messages.size() > 1 && (messages.size() > messageTarget || CharacterCount() > characterTarget))
    {
        // Conversation normally alternates user/assistant. Evict a complete old exchange
        // when possible so the retained context never begins with an orphaned answer.
        const bool completePair = messages.size() >= 2 && messages[0].role == "user" && messages[1].role == "assistant";
        messages.erase(messages.begin(), messages.begin() + (completePair ? 2 : 1));
        messageSources.erase(messageSources.begin(), messageSources.begin() + (completePair ? 2 : 1));
    }
}
