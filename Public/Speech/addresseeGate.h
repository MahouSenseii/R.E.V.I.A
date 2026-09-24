#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::speech
{

struct AddresseeSettings
{
    // Off answers everything the microphone hears, which is what hands-free used to do.
    bool requireWakeWord = true;
    // Whole words, matched case-insensitively. Include the ways speech recognition
    // spells her name back.
    std::vector<std::string> wakeWords = {"revia", "rivia", "revya", "reviya", "revea"};
    // After an exchange, follow-up speech is hers without repeating her name.
    int followUpSeconds = 20;
};

// Decides whether hands-free speech was meant for Revia.
//
// A room is full of speech that is not for her: a phone call, someone else, a video.
// Answering all of it made hands-free unusable while working, so speech counts only when
// it names her, or continues a conversation she is already in.
class AddresseeGate
{
public:
    using Clock = std::chrono::steady_clock;

    explicit AddresseeGate(AddresseeSettings settings = {});

    void Configure(AddresseeSettings settings);

    // Whether the transcript names her as a whole word.
    [[nodiscard]] static bool MentionsWakeWord(
        const std::string& transcript, const std::vector<std::string>& wakeWords);

    // True when speech heard at `now` is for her. During a call only her name counts,
    // because follow-up speech is then most likely meant for the other people.
    bool Accept(const std::string& transcript, Clock::time_point now, bool inCall);

    // Marks a moment she and the user were talking; follow-up speech is hers after it.
    void NoteExchange(Clock::time_point now);

private:
    mutable std::mutex mutex;
    AddresseeSettings settings;
    std::optional<Clock::time_point> lastExchange;
};

} // namespace revia::speech
