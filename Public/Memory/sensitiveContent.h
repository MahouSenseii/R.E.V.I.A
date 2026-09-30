#pragma once

#include <string>

namespace revia::memory
{

// What kind of secret was recognised.
//
// Named rather than a bare boolean because the classes differ in how they were found,
// and a reader deciding whether a refusal was right needs to know which rule fired.
// The value is never the secret and never part of it.
enum class SensitiveClass
{
    None,
    // The text says what it is carrying: "password", "api key", "recovery code". This
    // was the whole detector once, and it is the weakest class here -- it catches
    // people talking about credentials and misses people pasting them.
    NamedSecret,
    // A credential with an issuer's own prefix on it: sk-, ghp_, AKIA, xoxb-, AIza and
    // the rest. The prefix is the evidence; length and composition only stop an English
    // word from qualifying.
    VendorApiKey,
    // A PEM or OpenSSH private-key block.
    PrivateKeyBlock,
    // header.payload.signature, where the header begins with the base64url of '{"'.
    JsonWebToken,
    // An HTTP Authorization value. The structure is the evidence, not the word.
    BearerCredential,
    // scheme://user:secret@host -- a password carried inside a connection string.
    UrlEmbeddedPassword,
    // A Luhn-valid account number behind a recognised issuer prefix.
    PaymentCardNumber,
    // A United States social-security number by shape, with the never-issued ranges
    // excluded.
    NationalIdNumber
};

[[nodiscard]] std::string ToString(SensitiveClass value);

struct SensitiveFinding
{
    SensitiveClass kind = SensitiveClass::None;
    // What was recognised, in words -- "a GitHub personal access token" -- and never
    // any part of what was found. This string is safe to log; the text it came from is
    // not.
    std::string description;

    [[nodiscard]] explicit operator bool() const { return kind != SensitiveClass::None; }
};

// Shared durable-storage secret gate; model instructions are not enforcement.
// Detects issuer-prefixed keys with minimum-length letter/digit bodies, private-key armor and three-segment base64url JWTs starting eyJ.
// Also detects Authorization bearer values, scheme://user:secret@host and existing lexical markers.
// Cards require recognized issuer, 13-19 digits and Luhn; SSNs require 3-2-4 shape and valid ranges.
// Random strings are not secrets by entropy; unnamed recovery codes and unmarked passwords are not structurally detected.
[[nodiscard]] SensitiveFinding DetectSensitiveContent(const std::string& text);

// The same decision as a boolean, for the call sites that only need the gate.
[[nodiscard]] bool ContainsSensitiveContent(const std::string& text);

} // namespace revia::memory
