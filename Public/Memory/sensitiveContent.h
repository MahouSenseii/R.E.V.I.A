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

// The one detector that keeps a secret out of durable storage.
//
// The memory classifier, the durable store, the conversation archive and the desktop
// experience recorder all consult this. Several copies of the rules would eventually
// disagree, and the copy that disagreed by being weaker would be the one that wrote a
// credential to disk.
//
// WHAT IS DETECTED, and why each rule is safe to apply automatically:
//
//   * Vendor-prefixed API keys. A fixed table of issuer prefixes, each requiring a body
//     of a documented minimum length containing both letters and digits. The prefix is
//     what makes it a credential; nothing is inferred from randomness alone.
//   * PEM / OpenSSH private-key blocks, by their armour line.
//   * JSON Web Tokens, by three base64url segments whose first begins "eyJ".
//   * HTTP bearer credentials, by the Authorization value's structure.
//   * Passwords embedded in connection strings, by the scheme://user:secret@host shape.
//   * Payment card numbers: a recognised issuer prefix, a 13-19 digit length, and a
//     passing Luhn check -- three independent conditions, not one.
//   * United States social-security numbers, by the 3-2-4 shape with the ranges the
//     administration never issues excluded.
//   * The original lexical markers, unchanged.
//
// WHAT IS DELIBERATELY NOT DETECTED, and why:
//
//   * Long random-looking strings as a class. UUIDs, content hashes, commit ids, game
//     asset names and ordinary source constants are all high-entropy, and a rule that
//     refused them would make automatic memory useless in exactly the project this runs
//     inside. Entropy is not evidence of a secret.
//   * Recovery and backup codes by shape. The common forms -- "abcd-efgh-ijkl", eight
//     digits, five groups of four -- are indistinguishable from licence keys, order
//     references and game codes. They are caught only when the text names them, which
//     is the NamedSecret rule, and that gap is real rather than closed by guesswork.
//   * Passwords with no marker around them. A password is usually an ordinary-looking
//     string; there is nothing structural to find. The lexical markers remain the only
//     defence, and they are not a complete one.
//   * Anything the model was merely told not to remember. That instruction is defence
//     in depth. This function is the enforcement.
[[nodiscard]] SensitiveFinding DetectSensitiveContent(const std::string& text);

// The same decision as a boolean, for the call sites that only need the gate.
[[nodiscard]] bool ContainsSensitiveContent(const std::string& text);

} // namespace revia::memory
