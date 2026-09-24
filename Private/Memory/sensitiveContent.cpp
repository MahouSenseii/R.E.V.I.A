#include "Memory/sensitiveContent.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string_view>

namespace revia::memory
{

namespace
{

std::string Lowered(const std::string& text)
{
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return lowered;
}

bool IsTokenCharacter(const unsigned char character)
{
    return std::isalnum(character) != 0 || character == '_' || character == '-';
}

// True when `position` starts a word rather than continuing one. Without this, "task-"
// inside "risk-free" would be a prefix match, and every rule below would be looser than
// it reads.
bool AtTokenStart(const std::string& text, const std::size_t position)
{
    if (position == 0) return true;
    const unsigned char previous = static_cast<unsigned char>(text[position - 1]);
    return !IsTokenCharacter(previous) && previous != '.';
}

struct KeyBody
{
    std::size_t length = 0;
    bool hasLetter = false;
    bool hasDigit = false;
    bool allUpperAlphanumeric = true;
};

KeyBody MeasureBody(const std::string& text, const std::size_t from)
{
    KeyBody body;
    for (std::size_t index = from; index < text.size(); ++index)
    {
        const unsigned char character = static_cast<unsigned char>(text[index]);
        if (!IsTokenCharacter(character)) break;
        ++body.length;
        if (std::isalpha(character) != 0)
        {
            body.hasLetter = true;
            if (std::islower(character) != 0) body.allUpperAlphanumeric = false;
        }
        else if (std::isdigit(character) != 0) body.hasDigit = true;
        else body.allUpperAlphanumeric = false;
    }
    return body;
}

struct VendorPrefix
{
    std::string_view prefix;
    std::size_t minimumBody;
    // AWS key identifiers are a fixed width of upper-case alphanumerics, and saying so
    // is what keeps "AKIAsomething" in prose from qualifying.
    bool exactUpperCaseWidth;
    std::string_view description;
};

// Issuer prefixes, each with the shortest body that issuer actually emits. A prefix is
// the evidence; the length and composition checks only stop an ordinary hyphenated
// word from matching. Matched case-sensitively, because that is how every one of these
// is issued.
constexpr std::array<VendorPrefix, 22> VendorPrefixes = {{
    {"sk-ant-", 24, false, "an Anthropic API key"},
    {"sk-proj-", 20, false, "an OpenAI project API key"},
    {"sk-", 20, false, "an OpenAI-style secret key"},
    {"xai-", 20, false, "an xAI API key"},
    {"ghp_", 30, false, "a GitHub personal access token"},
    {"gho_", 30, false, "a GitHub OAuth token"},
    {"ghu_", 30, false, "a GitHub user-to-server token"},
    {"ghs_", 30, false, "a GitHub server-to-server token"},
    {"ghr_", 30, false, "a GitHub refresh token"},
    {"github_pat_", 40, false, "a GitHub fine-grained access token"},
    {"glpat-", 18, false, "a GitLab personal access token"},
    {"xoxb-", 20, false, "a Slack bot token"},
    {"xoxp-", 20, false, "a Slack user token"},
    {"xoxa-", 20, false, "a Slack app token"},
    {"xoxs-", 20, false, "a Slack session token"},
    {"AIza", 30, false, "a Google API key"},
    {"sk_live_", 16, false, "a Stripe live secret key"},
    {"rk_live_", 16, false, "a Stripe live restricted key"},
    {"hf_", 30, false, "a Hugging Face access token"},
    {"npm_", 30, false, "an npm access token"},
    {"dop_v1_", 40, false, "a DigitalOcean access token"},
    {"AKIA", 16, true, "an AWS access key identifier"},
}};

// The AWS family shares one width and one shape, so they are checked together rather
// than given a table row each.
constexpr std::array<std::string_view, 6> AwsKeyPrefixes = {
    "ASIA", "AROA", "AIDA", "ANPA", "ABIA", "ACCA"};

bool MatchesVendorPrefix(
    const std::string& text,
    const std::string_view prefix,
    const std::size_t minimumBody,
    const bool exactUpperCaseWidth)
{
    std::size_t position = text.find(prefix);
    while (position != std::string::npos)
    {
        if (AtTokenStart(text, position))
        {
            const KeyBody body = MeasureBody(text, position + prefix.size());
            const bool composed = body.hasLetter && body.hasDigit;
            if (exactUpperCaseWidth)
            {
                if (body.length == minimumBody && body.allUpperAlphanumeric && composed)
                {
                    return true;
                }
            }
            else if (body.length >= minimumBody && composed)
            {
                return true;
            }
        }
        position = text.find(prefix, position + 1);
    }
    return false;
}

std::string_view FindVendorKey(const std::string& text)
{
    for (const VendorPrefix& vendor : VendorPrefixes)
    {
        if (MatchesVendorPrefix(
                text, vendor.prefix, vendor.minimumBody, vendor.exactUpperCaseWidth))
        {
            return vendor.description;
        }
    }
    for (const std::string_view prefix : AwsKeyPrefixes)
    {
        if (MatchesVendorPrefix(text, prefix, 16, true))
        {
            return "an AWS access key identifier";
        }
    }
    return {};
}

// -----BEGIN ... PRIVATE KEY----- in any of its armour spellings. The word "private
// key" alone is a NamedSecret; the armour is what makes this the key itself.
bool ContainsPrivateKeyBlock(const std::string& lowered)
{
    std::size_t position = lowered.find("-----begin");
    while (position != std::string::npos)
    {
        const std::size_t lineEnd = std::min(lowered.size(), position + 80);
        const std::string_view armour(
            lowered.data() + position, lineEnd - position);
        if (armour.find("private key") != std::string_view::npos)
        {
            return true;
        }
        position = lowered.find("-----begin", position + 1);
    }
    return false;
}

bool IsBase64UrlCharacter(const unsigned char character)
{
    return std::isalnum(character) != 0 || character == '-' || character == '_';
}

std::size_t Base64UrlRun(const std::string& text, const std::size_t from)
{
    std::size_t length = 0;
    while (from + length < text.size() &&
        IsBase64UrlCharacter(static_cast<unsigned char>(text[from + length])))
    {
        ++length;
    }
    return length;
}

// "eyJ" is the base64url encoding of the two characters that open a JSON object, so a
// segment starting with it is a JOSE header and not a coincidence.
bool ContainsJsonWebToken(const std::string& text)
{
    std::size_t position = text.find("eyJ");
    while (position != std::string::npos)
    {
        if (AtTokenStart(text, position))
        {
            const std::size_t header = Base64UrlRun(text, position);
            std::size_t cursor = position + header;
            if (header >= 12 && cursor < text.size() && text[cursor] == '.')
            {
                const std::size_t payload = Base64UrlRun(text, cursor + 1);
                cursor += 1 + payload;
                if (payload >= 10 && cursor < text.size() && text[cursor] == '.')
                {
                    if (Base64UrlRun(text, cursor + 1) >= 10) return true;
                }
            }
        }
        position = text.find("eyJ", position + 1);
    }
    return false;
}

bool IsBearerTokenCharacter(const unsigned char character)
{
    return std::isalnum(character) != 0 || character == '-' || character == '_' ||
        character == '.' || character == '~' || character == '+' ||
        character == '/' || character == '=';
}

// An Authorization value, recognised by its structure. "Bearer <token>" in prose does
// not qualify, because the placeholder is not a token.
bool ContainsBearerCredential(const std::string& lowered, const std::string& text)
{
    std::size_t position = lowered.find("bearer ");
    while (position != std::string::npos)
    {
        std::size_t cursor = position + 7;
        while (cursor < text.size() && text[cursor] == ' ') ++cursor;
        std::size_t length = 0;
        bool hasLetter = false;
        bool hasDigit = false;
        while (cursor + length < text.size() &&
            IsBearerTokenCharacter(static_cast<unsigned char>(text[cursor + length])))
        {
            const unsigned char character =
                static_cast<unsigned char>(text[cursor + length]);
            if (std::isalpha(character) != 0) hasLetter = true;
            else if (std::isdigit(character) != 0) hasDigit = true;
            ++length;
        }
        if (length >= 20 && hasLetter && hasDigit) return true;
        position = lowered.find("bearer ", position + 1);
    }
    return false;
}

// scheme://user:secret@host. The authority ends at the first '/', '?', '#' or space, so
// a colon further along the path cannot be mistaken for a password separator.
bool ContainsUrlEmbeddedPassword(const std::string& text)
{
    std::size_t position = text.find("://");
    while (position != std::string::npos)
    {
        const std::size_t authority = position + 3;
        std::size_t end = authority;
        while (end < text.size() && text[end] != '/' && text[end] != '?' &&
            text[end] != '#' && std::isspace(static_cast<unsigned char>(text[end])) == 0)
        {
            ++end;
        }
        const std::string_view region(text.data() + authority, end - authority);
        const std::size_t at = region.find('@');
        if (at != std::string_view::npos)
        {
            const std::string_view userInfo = region.substr(0, at);
            const std::size_t colon = userInfo.find(':');
            if (colon != std::string_view::npos &&
                userInfo.size() - colon - 1 >= 6)
            {
                return true;
            }
        }
        position = text.find("://", position + 1);
    }
    return false;
}

bool PassesLuhn(const std::string& digits)
{
    int sum = 0;
    bool doubled = false;
    for (std::size_t index = digits.size(); index > 0; --index)
    {
        int value = digits[index - 1] - '0';
        if (doubled)
        {
            value *= 2;
            if (value > 9) value -= 9;
        }
        sum += value;
        doubled = !doubled;
    }
    return sum % 10 == 0;
}

// Recognised issuer ranges. Without this a Luhn-valid timestamp or order number would
// qualify, and roughly one in ten random digit strings passes Luhn.
bool HasIssuerPrefix(const std::string& digits)
{
    const auto leading = [&digits](const std::size_t count)
    {
        return digits.size() >= count ? std::stoi(digits.substr(0, count)) : -1;
    };
    if (digits.size() < 13 || digits.size() > 19) return false;
    if (digits[0] == '4' && (digits.size() == 13 || digits.size() == 16 ||
        digits.size() == 19)) return true;                      // Visa
    const int two = leading(2);
    if (two >= 51 && two <= 55 && digits.size() == 16) return true;   // MasterCard
    if ((two == 34 || two == 37) && digits.size() == 15) return true; // Amex
    if ((two == 36 || two == 38) && digits.size() >= 14) return true; // Diners
    const int four = leading(4);
    if (four == 6011 && digits.size() == 16) return true;             // Discover
    if (two == 65 && digits.size() == 16) return true;                // Discover
    if (four >= 2221 && four <= 2720 && digits.size() == 16) return true; // MasterCard
    return false;
}

// One run of digits and the separators people actually type between them. A run with
// doubled or edge separators is rejected, so two unrelated numbers side by side cannot
// be glued into a card-shaped one.
bool ContainsPaymentCardNumber(const std::string& text)
{
    std::size_t index = 0;
    while (index < text.size())
    {
        if (std::isdigit(static_cast<unsigned char>(text[index])) == 0)
        {
            ++index;
            continue;
        }
        std::size_t end = index;
        std::string digits;
        bool wellFormed = true;
        bool previousWasSeparator = false;
        while (end < text.size())
        {
            const char character = text[end];
            if (std::isdigit(static_cast<unsigned char>(character)) != 0)
            {
                digits.push_back(character);
                previousWasSeparator = false;
            }
            else if (character == ' ' || character == '-')
            {
                if (previousWasSeparator)
                {
                    wellFormed = false;
                    break;
                }
                previousWasSeparator = true;
            }
            else break;
            ++end;
        }
        // A trailing separator belongs to whatever follows, not to this number.
        if (previousWasSeparator) --end;
        const bool boundedRight = end >= text.size() ||
            !IsTokenCharacter(static_cast<unsigned char>(text[end]));
        if (wellFormed && boundedRight && AtTokenStart(text, index) &&
            HasIssuerPrefix(digits) && PassesLuhn(digits))
        {
            return true;
        }
        index = std::max(end, index + 1);
    }
    return false;
}

// The 3-2-4 shape, with the area, group and serial ranges the Social Security
// Administration never issues excluded. Those exclusions are most of what keeps an
// ordinary part number from qualifying.
bool ContainsNationalIdNumber(const std::string& text)
{
    for (std::size_t index = 0; index + 11 <= text.size(); ++index)
    {
        const auto digitsAt = [&text](const std::size_t from, const std::size_t count)
        {
            for (std::size_t offset = 0; offset < count; ++offset)
            {
                if (std::isdigit(static_cast<unsigned char>(text[from + offset])) == 0)
                {
                    return false;
                }
            }
            return true;
        };
        if (!digitsAt(index, 3) || text[index + 3] != '-' || !digitsAt(index + 4, 2) ||
            text[index + 6] != '-' || !digitsAt(index + 7, 4))
        {
            continue;
        }
        if (index > 0)
        {
            const unsigned char previous = static_cast<unsigned char>(text[index - 1]);
            if (std::isdigit(previous) != 0 || previous == '-') continue;
        }
        if (index + 11 < text.size())
        {
            const unsigned char next = static_cast<unsigned char>(text[index + 11]);
            if (std::isdigit(next) != 0 || next == '-') continue;
        }
        const int area = std::stoi(text.substr(index, 3));
        const int group = std::stoi(text.substr(index + 4, 2));
        const int serial = std::stoi(text.substr(index + 7, 4));
        if (area == 0 || area == 666 || area >= 900) continue;
        if (group == 0 || serial == 0) continue;
        return true;
    }
    return false;
}

std::string_view FindNamedSecret(const std::string& lowered)
{
    constexpr std::string_view Markers[] = {
        "password", "passcode", "api key", "secret key", "access token",
        "private key", "credit card", "social security", "recovery code"
    };
    for (const std::string_view marker : Markers)
    {
        if (lowered.find(marker) != std::string::npos) return marker;
    }
    return {};
}

} // namespace

std::string ToString(const SensitiveClass value)
{
    switch (value)
    {
        case SensitiveClass::NamedSecret: return "named_secret";
        case SensitiveClass::VendorApiKey: return "vendor_api_key";
        case SensitiveClass::PrivateKeyBlock: return "private_key_block";
        case SensitiveClass::JsonWebToken: return "json_web_token";
        case SensitiveClass::BearerCredential: return "bearer_credential";
        case SensitiveClass::UrlEmbeddedPassword: return "url_embedded_password";
        case SensitiveClass::PaymentCardNumber: return "payment_card_number";
        case SensitiveClass::NationalIdNumber: return "national_id_number";
        case SensitiveClass::None:
        default: return "none";
    }
}

SensitiveFinding DetectSensitiveContent(const std::string& text)
{
    if (text.empty()) return {};
    const std::string lowered = Lowered(text);

    // Structural rules first. They are the ones that catch a credential nobody
    // labelled, and their descriptions are more useful than "the word password appears"
    // when a refusal has to be explained.
    if (ContainsPrivateKeyBlock(lowered))
    {
        return {SensitiveClass::PrivateKeyBlock, "a private key block"};
    }
    if (const std::string_view vendor = FindVendorKey(text); !vendor.empty())
    {
        return {SensitiveClass::VendorApiKey, std::string(vendor)};
    }
    if (ContainsJsonWebToken(text))
    {
        return {SensitiveClass::JsonWebToken, "a JSON web token"};
    }
    if (ContainsBearerCredential(lowered, text))
    {
        return {SensitiveClass::BearerCredential, "an HTTP bearer credential"};
    }
    if (ContainsUrlEmbeddedPassword(text))
    {
        return {SensitiveClass::UrlEmbeddedPassword,
            "a password inside a connection string"};
    }
    if (ContainsPaymentCardNumber(text))
    {
        return {SensitiveClass::PaymentCardNumber, "a payment card number"};
    }
    if (ContainsNationalIdNumber(text))
    {
        return {SensitiveClass::NationalIdNumber, "a national identity number"};
    }
    if (const std::string_view named = FindNamedSecret(lowered); !named.empty())
    {
        return {SensitiveClass::NamedSecret,
            "text that names a credential (\"" + std::string(named) + "\")"};
    }
    return {};
}

bool ContainsSensitiveContent(const std::string& text)
{
    return static_cast<bool>(DetectSensitiveContent(text));
}

} // namespace revia::memory
