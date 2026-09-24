#include "Perception/clipboardText.h"

#include "Core/utf8.h"

#include <algorithm>
#include <array>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::perception
{

std::optional<ClipboardText> ReadClipboardText(const std::size_t maxBytes)
{
#ifdef _WIN32
    // Another app may hold the clipboard for a moment while it writes.
    bool opened = false;
    for (int attempt = 0; attempt < 5 && !opened; ++attempt)
    {
        opened = OpenClipboard(nullptr) != FALSE;
        if (!opened) Sleep(20);
    }
    if (!opened) return std::nullopt;
    struct Closer
    {
        ~Closer() { CloseClipboard(); }
    } closer;

    ClipboardText copied;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT) == FALSE) return copied;
    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data == nullptr) return copied;
    const auto* characters = static_cast<const wchar_t*>(GlobalLock(data));
    if (characters == nullptr) return copied;
    const std::size_t capacity = GlobalSize(data) / sizeof(wchar_t);
    std::size_t length = 0;
    while (length < capacity && characters[length] != L'\0') ++length;
    // Every UTF-16 unit is at most three UTF-8 bytes, so this reads enough and no more.
    const std::size_t read = std::min(length, maxBytes);
    std::string text;
    if (read > 0)
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, characters, static_cast<int>(read),
            nullptr, 0, nullptr, nullptr);
        if (size > 0)
        {
            text.resize(static_cast<std::size_t>(size));
            WideCharToMultiByte(CP_UTF8, 0, characters, static_cast<int>(read), text.data(),
                size, nullptr, nullptr);
        }
    }
    GlobalUnlock(data);

    std::string normalized;
    normalized.reserve(text.size());
    for (const char character : text)
    {
        if (character != '\r') normalized.push_back(character);
    }
    copied.truncated = read < length || normalized.size() > maxBytes;
    copied.text = utf8::Prefix(normalized, maxBytes);
    return copied;
#else
    (void)maxBytes;
    return std::nullopt;
#endif
}

bool AsksAboutClipboard(const std::string& input)
{
    // Words only, one space apart and padded, so phrases match whole words.
    std::string words = " ";
    for (const unsigned char character : input)
    {
        if (std::isalpha(character) != 0 || character == '\'')
        {
            words.push_back(static_cast<char>(std::tolower(character)));
        }
        else if (words.back() != ' ')
        {
            words.push_back(' ');
        }
    }
    if (words.back() != ' ') words.push_back(' ');
    static const std::array<const char*, 7> phrases = {
        " clipboard ", " clipboard's ", " just copied ", " copied text ", " what's copied ",
        " whats copied ", " the copied "};
    for (const char* phrase : phrases)
    {
        if (words.find(phrase) != std::string::npos) return true;
    }
    // "I copied" alone is as often about files or the past; "the code I copied" is not.
    static const std::array<const char*, 18> things = {
        "what", "code", "text", "link", "url", "thing", "stuff", "error", "message",
        "paragraph", "email", "snippet", "quote", "list", "table", "line", "lines", "sentence"};
    for (const char* thing : things)
    {
        for (const char* copied : {" i copied ", " i've copied ", " i have copied "})
        {
            if (words.find(" " + std::string(thing) + copied) != std::string::npos) return true;
        }
    }
    return false;
}

} // namespace revia::perception
