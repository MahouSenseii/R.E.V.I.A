#pragma once

// Controls answer completeness without changing identity, model routing or earned state.
// Every mode must preserve runtime-confirmed outcomes.
enum class AnswerObligationMode
{
    // Supply available substance; character shapes delivery without replacing the answer.
    Reliable,
    // The recommended default. Useful by default, with real room to tease first, answer
    // partly, or decline when her state and the moment genuinely call for it.
    Balanced,
    // Character may outweigh completeness in ordinary low-stakes conversation without requiring refusal.
    CharacterFirst
};
