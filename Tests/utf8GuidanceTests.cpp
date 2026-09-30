#include "testSupport.h"
#include "Agents/conversationQualityMonitor.h"
#include "Agents/conversationStylePolicy.h"
#include "Core/utf8.h"

#include <iostream>
#include <nlohmann/json.hpp>

void RunUtf8GuidanceTests()
{
    using revia::tests::Check;
    using json = nlohmann::json;
    for (const std::string& unit : {std::string("\xE4\xB8\xAD"), std::string("\xF0\x9F\x98\x80"), std::string("e\xCC\x81")})
    {
        std::string reply = "x";
        for (int index = 0; index < 300; ++index) reply += unit;
        const std::string input = "Continue the discussion.";
        const auto guidance = revia::agents::ConversationStylePolicy{}.BuildTurnGuidance(
            input, {{"assistant", reply}, {"user", input}});
        Check(revia::utf8::IsValid(guidance), "Recent reply guidance split a UTF-8 code point.");
        Check(guidance.find(unit) != std::string::npos, "Recent reply guidance lost valid multilingual content.");
        Check(json::parse(json(guidance).dump()).get<std::string>() == guidance,
            "Recent reply guidance did not round-trip through JSON.");

        const auto opening = revia::agents::ConversationQualityMonitor::OpeningOf(reply);
        Check(revia::utf8::IsValid(opening) && opening.size() <= 96 && reply.starts_with(opening),
            "The repetition monitor split a UTF-8 code point or exceeded its opening budget.");
        Check(!json(opening).dump().empty(), "The repetition opening was not JSON-safe.");
    }
    const auto ascii = revia::agents::ConversationQualityMonitor::OpeningOf("HELLO. Next sentence.");
    Check(ascii == "hello", "The UTF-8 fix changed ASCII opening normalization.");
    std::cout << "Multilingual reply guidance and repetition openings preserve UTF-8.\n";
}
