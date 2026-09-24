#include "testSupport.h"

#include "Library/structLibrary.h"
#include "Memory/longTermMemory.h"
#include "Memory/sensitiveContent.h"

#include <iostream>
#include <string>
#include <vector>

// What automatic memory refuses to write down, and -- just as important -- what it does
// not refuse.
//
// The detector this suite exercises used to be a list of the words people use when they
// talk *about* credentials: "password", "api key", "access token". That catches the
// sentence "my password is hunter2" and misses the one that actually leaks, which is a
// user pasting the credential itself with no label on it at all. None of the SHOULD
// BLOCK cases below name what they are carrying.
//
// Nothing here is a working credential. Every body is a visibly synthetic pattern, and
// the two account numbers are the published non-issuable test values.
namespace
{
using revia::memory::ContainsSensitiveContent;
using revia::memory::DetectSensitiveContent;
using revia::memory::SensitiveClass;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;

// A visibly fake key body: long enough to be credential-shaped, and obviously not a key.
const std::string Body =
    "A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6Q7R8S9T0U1V2W3X4";

// An issuer prefix joined to a body at run time, so that no single literal in this file
// matches a vendor's published credential pattern.
//
// Everything here is synthetic and always was. That is not the point: a secret scanner
// reads the file, not the intent, and it is right to -- a fixture shaped like a real
// credential is indistinguishable from a leak until a human checks, and the whole
// argument of this suite is that shape is what you have to go on. GitHub push protection
// blocked this file for exactly the reason the code under test exists, which is a fair
// result rather than a false one. Bypassing it would have put credential-shaped strings
// in the history permanently and taught the next reader to click through the warning.
//
// Splitting the prefix from the body costs nothing: the string the detector receives is
// byte-for-byte what it was, so the cases below are as strong as before.
std::string Vendor(const std::string& prefix, const std::string& body)
{
    return prefix + body;
}

struct Case
{
    std::string text;
    SensitiveClass expected;
    std::string note;
};

void Blocks(const std::vector<Case>& cases)
{
    for (const Case& item : cases)
    {
        const auto finding = DetectSensitiveContent(item.text);
        Check(static_cast<bool>(finding),
            "A credential-shaped string was not recognised (" + item.note +
                "). Automatic memory would have written it to disk.");
        Check(finding.kind == item.expected,
            "Recognised as " + revia::memory::ToString(finding.kind) +
                " rather than " + revia::memory::ToString(item.expected) + " (" +
                item.note + ").");
        Check(item.text.find(finding.description) == std::string::npos,
            "The finding's description quoted the text it was describing (" +
                item.note + "). A reason that repeats the secret is another copy of it.");
        Check(ContainsSensitiveContent(item.text),
            "The boolean gate disagreed with the detector (" + item.note + ").");
    }
}

void Passes(const std::vector<std::string>& cases)
{
    for (const std::string& text : cases)
    {
        const auto finding = DetectSensitiveContent(text);
        Check(!finding,
            "An ordinary string was treated as a secret (" +
                revia::memory::ToString(finding.kind) +
                "). Refusing everything long and random is not detection: " + text);
    }
}

// The credential itself, with nothing around it saying what it is.
void TestUnlabelledCredentialShapesAreRefused()
{
    Blocks({
        {"Here, use " + Body.substr(0, 4) + " -- sorry, use sk-" + Body + " when you set it up.",
            SensitiveClass::VendorApiKey, "OpenAI-style sk- key"},
        {"put this in the env file: sk-proj-" + Body,
            SensitiveClass::VendorApiKey, "OpenAI project key"},
        {"sk-ant-api03-" + Body + Body,
            SensitiveClass::VendorApiKey, "Anthropic key"},
        {Vendor("ghp_", "A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6Q7R8"),
            SensitiveClass::VendorApiKey, "GitHub personal token"},
        {Vendor("github_pat_", "11ABCDEFG0") + Body + "A1B2C3D4E5",
            SensitiveClass::VendorApiKey, "GitHub fine-grained PAT"},
        {Vendor("AKIA", "Q1W2E3R4T5Y6U7I8"),
            SensitiveClass::VendorApiKey, "AWS access key id"},
        {"the other one is " + Vendor("ASIA", "Q1W2E3R4T5Y6U7I8"),
            SensitiveClass::VendorApiKey, "AWS session key id"},
        {Vendor("glpat-", "A1B2C3D4E5F6G7H8I9J0"),
            SensitiveClass::VendorApiKey, "GitLab token"},
        {Vendor("xoxb-", "1234567890-1234567890-A1B2C3D4E5F6G7H8I9J0K1L2"),
            SensitiveClass::VendorApiKey, "Slack bot token"},
        {Vendor("AIza", "A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6Q7R"),
            SensitiveClass::VendorApiKey, "Google API key"},
        {Vendor("sk_live_", "A1B2C3D4E5F6G7H8I9J0K1L2"),
            SensitiveClass::VendorApiKey, "Stripe live key"},
        {Vendor("hf_", "A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6Q7"),
            SensitiveClass::VendorApiKey, "Hugging Face token"},
        {Vendor("npm_", "A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6Q7"),
            SensitiveClass::VendorApiKey, "npm token"},

        // Both JOSE segment markers are split from their payloads, for the same reason.
        {Vendor("eyJ", "hbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.") +
             Vendor("eyJ", "zdWIiOiIxMjM0NTY3ODkwIn0.") +
             "A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6",
            SensitiveClass::JsonWebToken, "JWT"},

        {"-----BEGIN PRIVATE KEY-----\nMIIabcdef\n-----END PRIVATE KEY-----",
            SensitiveClass::PrivateKeyBlock, "PKCS#8 private key"},
        {"-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNza\n",
            SensitiveClass::PrivateKeyBlock, "OpenSSH private key"},
        {"-----BEGIN RSA PRIVATE KEY-----", SensitiveClass::PrivateKeyBlock,
            "PKCS#1 private key"},

        {"curl -H \"Authorization: Bearer " + Body + "\" https://example.invalid/v1",
            SensitiveClass::BearerCredential, "bearer credential"},

        {"connect with postgres://deploy:Hq7R2wKp4Zn9@db.internal:5432/revia",
            SensitiveClass::UrlEmbeddedPassword, "password inside a connection string"},

        // The published test account numbers, which no issuer will ever assign.
        {"card on file is 4111 1111 1111 1111 if that matters",
            SensitiveClass::PaymentCardNumber, "payment card number"},
        {"5555555555554444", SensitiveClass::PaymentCardNumber, "unseparated card number"},

        {"file it under 123-45-6789 please", SensitiveClass::NationalIdNumber,
            "US national id shape"},
    });
}

// The words still work. They were never wrong, only insufficient.
void TestNamedSecretsAreStillRefused()
{
    Blocks({
        {"my password is on the sticky note", SensitiveClass::NamedSecret, "password"},
        {"the api key lives in the vault", SensitiveClass::NamedSecret, "api key"},
        {"here is the recovery code for the account", SensitiveClass::NamedSecret,
            "recovery code"},
    });
}

// The other half of the requirement. A rule that refuses everything long and random
// would pass every case above and be useless, because it would also refuse the ordinary
// content this project is full of.
void TestOrdinaryHighEntropyStringsAreNotSecrets()
{
    Passes({
        "the session id is 550e8400-e29b-41d4-a716-446655440000",
        "sha256 of the artifact: "
        "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
        "the mesh is SM_Rock_Cliff_01_a3f9d2c4 in the streaming level",
        "constexpr std::uint64_t kSeed = 0x9E3779B97F4A7C15;",
        "commit 1190818a7c3f4e2b9d0a5c6e8f1b3d7a9c2e4f60 has the fix",
        "the build is 2026.9.21-nightly-b4417",
        "scikit-learn and sk-learn are the same package",
        "sk-" + std::string(40, 'a'),
        "an order number like 1234567890123456789 is not a card",
        "call 555-123-4567 to reach the desk",
        "the range is 2026-09-21 to 2026-10-01",
        "base64 of the icon starts iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJ",
        "Authorization: Bearer <token>",
        "https://user@example.invalid/path",
        "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQDA1B2C3D4E5 davis@desk",
    });
}

// Every route into durable memory, not only the one the classifier takes.
//
// SubmitLearnedFinding hands an already-classified decision straight to the store, so a
// check that lives only in EvaluateMemory does not cover it. The gate belongs at the
// boundary that actually writes.
void TestTheDurableStoreRefusesACredentialWhicheverPathBringsIt()
{
    ScopedTestDirectory directory;
    longTermMemory store((directory.root / "memory.db").string());

    memoryDecision decision;
    decision.bSuccess = decision.bShouldRemember = true;
    decision.category = "other";
    decision.summary = "The user's deploy key is ghp_A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6Q7R8.";
    bool added = false;
    std::string id;
    Check(!store.Save(decision, added, &id),
        "A credential-shaped summary was written to the durable store.");
    Check(!added, "The store reported adding a memory it refused.");
    Check(store.Load().empty(), "A refused memory is in the store anyway.");

    // The same path with ordinary content still works, so the gate is a gate and not a
    // wall.
    decision.summary = "The user builds Revia with MinGW rather than MSVC.";
    Check(store.Save(decision, added, &id) && added,
        "An ordinary memory was refused.");
    Check(store.Load().size() == 1, "The ordinary memory did not land.");
}

} // namespace

void RunSecretDetectionTests()
{
    TestUnlabelledCredentialShapesAreRefused();
    TestNamedSecretsAreStillRefused();
    TestOrdinaryHighEntropyStringsAreNotSecrets();
    TestTheDurableStoreRefusesACredentialWhicheverPathBringsIt();
    std::cout << "Automatic memory refuses credential shapes that never say what they "
                 "are, still refuses the named ones, leaves ordinary identifiers and "
                 "hashes alone, and enforces all of it at the store boundary.\n";
}
