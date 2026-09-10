#pragma once

#include "Performance/songTypes.h"

#include <filesystem>
#include <string>
#include <vector>

namespace revia::performance
{

// Reads what is in the songs folder. It never writes there and never reaches outside it.
//
// A song is a directory under the library root:
//
//   RuntimeData/Songs/<id>/
//     song.json          optional - title, credit, gains, sections
//     instrumental.wav   optional
//     vocal.wav          optional
//
// At least one of the two tracks must exist. A folder holding a single .wav and no
// song.json is still a song -- dropping one file in and asking her to sing it should
// work without writing any configuration first.
class SongLibrary
{
public:
    void SetRoot(std::filesystem::path root);
    [[nodiscard]] const std::filesystem::path& Root() const;

    // Every folder, including the ones that cannot be performed. A broken song that
    // silently fails to appear is indistinguishable from one that was never added, so
    // the problem travels with the entry instead.
    [[nodiscard]] std::vector<SongSummary> List() const;

    [[nodiscard]] bool Load(
        const std::string& songId,
        SongAsset& outAsset,
        std::string& outError) const;

    // Matches an id exactly first, then a unique case-insensitive title or id prefix, so
    // "/sing bright" finds the one song whose title starts that way. Ambiguity is
    // reported rather than guessed at.
    [[nodiscard]] bool Resolve(
        const std::string& query,
        std::string& outSongId,
        std::string& outError) const;

    // A plain folder name: no separators, no drive letters, no dot entries. The id comes
    // from a chat message, so it is treated as untrusted text and never joined to a path
    // until it has passed this.
    [[nodiscard]] static bool IsSafeSongId(const std::string& songId);

private:
    [[nodiscard]] SongSummary Inspect(const std::filesystem::directory_entry& entry) const;

    std::filesystem::path root;
};

} // namespace revia::performance
