#include "Audit/contentDigest.h"

#include <stdexcept>
#include <string>

void RunContentDigestTests()
{
    const auto check = [](const std::string& input, const std::string& expected)
    {
        if (revia::audit::ContentDigest(input) != expected)
            throw std::runtime_error("Exact content SHA256 does not match the independent fixture.");
    };
    check("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check(std::string(1000, 'a'), "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    if (revia::audit::ContentDigest(std::string("a\0b", 3)) == revia::audit::ContentDigest("a"))
        throw std::runtime_error("Content digest discarded embedded NUL bytes.");
}

#ifdef REVIA_CONTENT_DIGEST_STANDALONE
int main() { RunContentDigestTests(); }
#endif
