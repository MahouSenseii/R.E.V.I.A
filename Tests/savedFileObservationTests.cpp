#include "Perception/savedFileObservation.h"
#include "Audit/contentDigest.h"

#include <iostream>
#include <stdexcept>

namespace
{
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
}

void RunSavedFileObservationTests()
{
    using namespace std::chrono_literals;
    using namespace revia::perception;
    const auto now = std::chrono::system_clock::now();
    SavedFileObservation source{std::filesystem::current_path(), std::filesystem::current_path() / "sample.cpp", {},
        "const char* Label() { return \"steady\"; }\n", now};
    source.revision = revia::audit::ContentDigest(source.contents);
    std::string reason;
    Require(ValidateSavedFileObservation(source, now, reason), "A current, complete saved C++ revision was refused.");
    const auto context = SavedFileContext(source);
    Require(context.find(source.revision) != std::string::npos && context.find(source.contents) != std::string::npos &&
        context.find("saved") != std::string::npos && context.find("untrusted") != std::string::npos,
        "Saved context lost exact source or its evidence boundary.");
    auto changed = source;
    changed.contents += "// changed after capture\n";
    Require(!ValidateSavedFileObservation(changed, now, reason), "A digest/content mismatch was admitted.");
    changed = source; changed.occurredAt = now - 121s;
    Require(!ValidateSavedFileObservation(changed, now, reason), "A stale saved revision was admitted.");
    changed = source; changed.occurredAt = now + 6s;
    Require(!ValidateSavedFileObservation(changed, now, reason), "A future observation was admitted.");
    changed = source; changed.path = source.projectRoot.parent_path() / "outside.cpp";
    Require(!ValidateSavedFileObservation(changed, now, reason), "A file outside the declared project was admitted.");
    changed = source; changed.path = source.projectRoot / "secret.env";
    Require(!ValidateSavedFileObservation(changed, now, reason), "An unsupported private file type was admitted.");
    changed = source; changed.contents = "int unfinished() {\n"; changed.revision = revia::audit::ContentDigest(changed.contents);
    Require(!ValidateSavedFileObservation(changed, now, reason), "Incomplete saved typing caused a review nomination.");
    changed = source; changed.contents = "int f() { /* partial"; changed.revision = revia::audit::ContentDigest(changed.contents);
    Require(!ValidateSavedFileObservation(changed, now, reason), "An unfinished comment caused a review nomination.");
    changed = source; changed.contents = "const char* f() { return \"} not code {\"; } // {\n";
    changed.revision = revia::audit::ContentDigest(changed.contents);
    Require(ValidateSavedFileObservation(changed, now, reason), "Braces in strings/comments were treated as active code.");
    for (const std::string contents :
        {"int f() { return 1'000; }\n", "int f() { return 0xAB'CD + 0b10'01; }\n", "double f() { return 1'234.5e-2 + .1'2; }\n",
            "char f() { return L'{' == '{' ? '\\'' : '}'; }\n", "const char* text = R\"(one double quote: \" [ { // /*)\";\n",
            "const auto* text = u8R\"tag(quoted \" )\" [ {)tag\";\n", "const char* text = R\"abcdefghijklmnop(\" [)abcdefghijklmnop\";\n",
            "#define PREFIX_R\nconst char* text = PREFIX_R\"ordinary\";\n"})
    {
        changed = source;
        changed.contents = contents;
        changed.revision = revia::audit::ContentDigest(changed.contents);
        Require(ValidateSavedFileObservation(changed, now, reason),
            ("A complete numeric or string literal suppressed saved-file awareness: " + contents).c_str());
    }
    for (const std::string contents : {"int f() { return 1'; }\n", "char f() { return L'{; }\n", "const char* text = R\"(unfinished\";\n",
             "const char* text = R\"tag(unfinished)other\";\n", "const char* text = R\"abcdefghijklmnopq(text)abcdefghijklmnopq\";\n",
             "const char* text = R\"bad tag(text)bad tag\";\n", "const char* text = R\"(complete)\"; int f() {\n",
             "#define PREFIX_R\nconst char* text = PREFIX_R\"(\"; int f() { // )\"\n"})
    {
        changed = source;
        changed.contents = contents;
        changed.revision = revia::audit::ContentDigest(changed.contents);
        Require(
            !ValidateSavedFileObservation(changed, now, reason), "An unfinished literal or source fragment caused a review nomination.");
    }
    changed = source;
    changed.contents = std::string(16385, 'x');
    changed.revision = revia::audit::ContentDigest(changed.contents);
    Require(!ValidateSavedFileObservation(changed, now, reason), "An oversized source snapshot was admitted.");
    changed = source;
    changed.contents = "const char* key = \"-----BEGIN PRIVATE KEY-----\";\n";
    changed.revision = revia::audit::ContentDigest(changed.contents);
    Require(!ValidateSavedFileObservation(changed, now, reason), "An obvious private key entered optional generation context.");
}

#ifdef REVIA_SAVED_FILE_TEST_MAIN
int main()
{
    try { RunSavedFileObservationTests(); std::cout << "Saved-file observation tests passed.\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
#endif
