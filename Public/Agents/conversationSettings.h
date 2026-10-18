#pragma once

#include <string>

// Durable conversation history. A separate block from memory because it is a separate
// promise: memory keeps facts a classifier judged worth having, this keeps what was said.
struct conversationSettings
{
    bool bArchiveEnabled = true;
    // Ceilings, not targets. An archive that grows without bound becomes a liability the
    // user never agreed to keep.
    int maxSessions = 200;
    int maxTurnsPerSession = 500;
    int maxTurnCharacters = 8000;
    // Replay this many previous-session turns at startup; retained turns consume prompt tokens.
    int restoreTurns = 6;
    // Allow one bounded self-inquiry on turns judged difficult by the intelligence router.
    bool bSelfInquiryEnabled = true;
    // Turns that must pass before she may think out loud again. Deliberation on every
    // hard turn in a row stops being thinking and becomes a preamble.
    int selfInquiryCooldownTurns = 3;
    // "hard" covers Expert/deep-reasoning turns; "questions" also covers ordinary questions and tasks.
    std::string selfInquiryScope = "hard";
    // How a turn that should think does it. "native": the answer itself runs with the
    // model's own thinking, streamed to "Revia is thinking", and no separate inquiry
    // call is made. "selfInquiry": the separate bounded question-and-answer pass.
    std::string deliberation = "native";
    // Whether Revia may hand a bounded, read-only investigation from chat to her Agent Studio
    // workers. "off": no delegation tools. "on_request": only when the person asks her to
    // ("use your agents to check this"). "auto": she may also choose to on her own. Choosing
    // to delegate never grants the workers more than reading inside approved folders.
    std::string delegation = "on_request";
    // Allow further verification rounds when inquiry leaves material questions unsettled.
    // This controls work independently of whether summaries are displayed.
    bool bIterativeInvestigationEnabled = false;
    // Bounds. Reaching any of them pauses the investigation; none of them makes it
    // succeed.
    int investigationMaximumRounds = 3;
    int investigationQuestionsPerRound = 2;
    int investigationBudgetMilliseconds = 45000;
};
