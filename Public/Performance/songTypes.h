#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace revia::performance
{

// Singing is a performance, not a long sentence.
//
// Ordinary speech and a song want different things from the audio path. Speech is a
// queue of short phrases that must stay in order and must be interruptible mid-word; a
// song is one continuous timeline where two tracks have to stay sample-aligned for
// minutes. Trying to serve both from the speech queue would have meant weakening the
// ordering guarantees that make speech work, so this is a separate owner with its own
// playback -- and a failure in here is designed to be survivable by everything else.
//
// What it is not: it is not a singing engine. Revia performs audio that already exists,
// which is what DECISION-REVIA-0006 chose as the first dependable form. When a real
// ISingingEngine arrives it replaces where the vocal samples come from, not any of the
// mixing, timing, or event machinery below.

enum class PerformanceState
{
    Idle,
    // Reading and mixing. Separate from Performing because a long song takes a
    // noticeable moment to load, and "nothing happened yet" is a different thing to
    // report than "she is singing".
    Preparing,
    Performing,
    Stopping
};

// One marked moment in a song. `label` is the section ("Verse", "Chorus") and `line` is
// the karaoke line to show while it is current. Both are the song author's own text,
// carried through as opaque data: nothing here writes, generates, or interprets lyrics.
struct SongSection
{
    std::int64_t startMs = 0;
    std::string label;
    std::string line;
};

// A stretch of the vocal track that actually contains singing, measured from the audio
// rather than declared in metadata. A mouth that opens because a file said so will
// eventually be wrong; one that opens because the samples are loud is right by
// construction.
struct VocalSpan
{
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
};

struct SongMetadata
{
    std::string id;
    std::string title;
    // Credit for the work being performed, as supplied by whoever assembled the asset.
    std::string artist;
    std::string notes;
    double instrumentalGain = 1.0;
    double vocalGain = 1.0;
    std::vector<SongSection> sections;
};

// A song after it has been read off disk and checked, but before any audio is mixed.
struct SongAsset
{
    SongMetadata metadata;
    std::filesystem::path directory;
    std::filesystem::path instrumentalPath;
    std::filesystem::path vocalPath;
    int sampleRate = 0;
    std::int64_t durationMs = 0;
    // Either track may be absent. An instrumental with no vocal is a backing track she
    // plays; a vocal with no instrumental is her singing unaccompanied. Both are songs.
    bool hasInstrumental = false;
    bool hasVocal = false;
};

// What the library can say about a folder without loading its audio.
struct SongSummary
{
    std::string id;
    std::string title;
    std::string artist;
    bool hasInstrumental = false;
    bool hasVocal = false;
    bool usable = false;
    // Why not, when usable is false. Shown to the user rather than swallowed, because a
    // song that silently fails to appear is indistinguishable from one never added.
    std::string problem;
};

struct PerformanceStatus
{
    PerformanceState state = PerformanceState::Idle;
    std::string songId;
    std::string title;
    std::string artist;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
    std::string sectionLabel;
    // The karaoke line currently being sung, empty when the song marks none.
    std::string line;
    bool vocalActive = false;
};

enum class PerformanceEventKind
{
    SongStarted,
    SectionStarted,
    VocalStarted,
    VocalEnded,
    SongEnded,
    SongInterrupted,
    SongFailed
};

struct PerformanceEvent
{
    PerformanceEventKind kind = PerformanceEventKind::SongStarted;
    std::string songId;
    std::string title;
    std::string label;
    std::string line;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
    std::string message;
};

[[nodiscard]] std::string ToString(PerformanceState value);
[[nodiscard]] std::string ToString(PerformanceEventKind value);
// mm:ss for a millisecond position, so every surface formats a song time the same way.
[[nodiscard]] std::string FormatSongTime(std::int64_t milliseconds);

} // namespace revia::performance
