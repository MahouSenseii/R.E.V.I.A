#include "Performance/songLibrary.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

namespace revia::performance
{

namespace
{

using json = nlohmann::json;

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool HasWavExtension(const std::filesystem::path& path)
{
    return Lower(path.extension().string()) == ".wav";
}

// Bounded so a hand-edited or generated file cannot turn a song into a memory problem
// or a wall of text on the karaoke line.
constexpr std::size_t MaximumSections = 512;
constexpr std::size_t MaximumTextBytes = 300;

std::string BoundedText(const json& data, const char* key, const std::size_t limit)
{
    if (!data.contains(key) || !data[key].is_string())
    {
        return {};
    }
    std::string value = data[key].get<std::string>();
    if (value.size() > limit)
    {
        value.resize(limit);
    }
    // Section labels and karaoke lines are displayed, so a stray newline would break the
    // line they are drawn on.
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value;
}

double BoundedGain(const json& data, const char* key, const double fallback)
{
    if (!data.contains(key) || !data[key].is_number())
    {
        return fallback;
    }
    return std::clamp(data[key].get<double>(), 0.0, 2.0);
}

// The named files first, then any single .wav in the folder. The second rule is what
// makes "drop a file in and sing it" work.
void FindTracks(
    const std::filesystem::path& directory,
    std::filesystem::path& outInstrumental,
    std::filesystem::path& outVocal)
{
    std::error_code error;
    const auto named = [&](const char* name)
    {
        const std::filesystem::path candidate = directory / name;
        return std::filesystem::is_regular_file(candidate, error) ? candidate
                                                                  : std::filesystem::path{};
    };
    outInstrumental = named("instrumental.wav");
    if (outInstrumental.empty()) outInstrumental = named("backing.wav");
    outVocal = named("vocal.wav");
    if (outVocal.empty()) outVocal = named("vocals.wav");
    if (!outInstrumental.empty() || !outVocal.empty())
    {
        return;
    }

    std::vector<std::filesystem::path> loose;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (entry.is_regular_file(error) && HasWavExtension(entry.path()))
        {
            loose.push_back(entry.path());
        }
    }
    // Exactly one, deliberately. With two unnamed files there is no way to tell which is
    // the backing track and which is the voice, and guessing wrong is a bad sound.
    if (loose.size() == 1)
    {
        outVocal = loose.front();
    }
}

} // namespace

void SongLibrary::SetRoot(std::filesystem::path inputRoot)
{
    root = std::move(inputRoot);
}

const std::filesystem::path& SongLibrary::Root() const
{
    return root;
}

bool SongLibrary::IsSafeSongId(const std::string& songId)
{
    if (songId.empty() || songId.size() > 64 || songId == "." || songId == "..")
    {
        return false;
    }
    return std::all_of(songId.begin(), songId.end(), [](const unsigned char character)
    {
        return std::isalnum(character) || character == '-' || character == '_' ||
            character == ' ' || character == '.';
    });
}

SongSummary SongLibrary::Inspect(const std::filesystem::directory_entry& entry) const
{
    SongSummary summary;
    summary.id = entry.path().filename().string();
    summary.title = summary.id;
    if (!IsSafeSongId(summary.id))
    {
        summary.problem = "The folder name cannot be used as a song id.";
        return summary;
    }

    std::error_code error;
    const std::filesystem::path descriptor = entry.path() / "song.json";
    if (std::filesystem::is_regular_file(descriptor, error))
    {
        try
        {
            std::ifstream file(descriptor);
            json data;
            file >> data;
            if (data.is_object())
            {
                const std::string title = BoundedText(data, "title", MaximumTextBytes);
                if (!title.empty()) summary.title = title;
                summary.artist = BoundedText(data, "artist", MaximumTextBytes);
            }
        }
        catch (const std::exception&)
        {
            summary.problem = "song.json could not be read as JSON.";
            return summary;
        }
    }

    std::filesystem::path instrumental;
    std::filesystem::path vocal;
    FindTracks(entry.path(), instrumental, vocal);
    summary.hasInstrumental = !instrumental.empty();
    summary.hasVocal = !vocal.empty();
    if (!summary.hasInstrumental && !summary.hasVocal)
    {
        summary.problem = "No instrumental.wav or vocal.wav, and no single .wav to use.";
        return summary;
    }
    summary.usable = true;
    return summary;
}

std::vector<SongSummary> SongLibrary::List() const
{
    std::vector<SongSummary> songs;
    std::error_code error;
    if (root.empty() || !std::filesystem::is_directory(root, error))
    {
        return songs;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, error))
    {
        if (!entry.is_directory(error))
        {
            continue;
        }
        songs.push_back(Inspect(entry));
    }
    std::sort(songs.begin(), songs.end(), [](const SongSummary& first, const SongSummary& second)
    {
        return Lower(first.title) < Lower(second.title);
    });
    return songs;
}

bool SongLibrary::Load(
    const std::string& songId,
    SongAsset& outAsset,
    std::string& outError) const
{
    outAsset = {};
    if (!IsSafeSongId(songId))
    {
        outError = "That is not a usable song name.";
        return false;
    }
    if (root.empty())
    {
        outError = "No song library folder is configured.";
        return false;
    }

    std::error_code error;
    const std::filesystem::path directory = root / songId;
    if (!std::filesystem::is_directory(directory, error))
    {
        outError = "There is no song called " + songId + ".";
        return false;
    }

    outAsset.directory = directory;
    outAsset.metadata.id = songId;
    outAsset.metadata.title = songId;

    const std::filesystem::path descriptor = directory / "song.json";
    if (std::filesystem::is_regular_file(descriptor, error))
    {
        try
        {
            std::ifstream file(descriptor);
            json data;
            file >> data;
            if (!data.is_object())
            {
                outError = "song.json must contain a JSON object.";
                return false;
            }
            const std::string title = BoundedText(data, "title", MaximumTextBytes);
            if (!title.empty()) outAsset.metadata.title = title;
            outAsset.metadata.artist = BoundedText(data, "artist", MaximumTextBytes);
            outAsset.metadata.notes = BoundedText(data, "notes", MaximumTextBytes);
            outAsset.metadata.instrumentalGain = BoundedGain(data, "instrumentalGain", 1.0);
            outAsset.metadata.vocalGain = BoundedGain(data, "vocalGain", 1.0);
            if (data.contains("sections") && data["sections"].is_array())
            {
                for (const auto& section : data["sections"])
                {
                    if (outAsset.metadata.sections.size() >= MaximumSections)
                    {
                        break;
                    }
                    if (!section.is_object()) continue;
                    SongSection parsed;
                    if (section.contains("startMs") && section["startMs"].is_number_integer())
                    {
                        parsed.startMs = std::max<std::int64_t>(
                            0, section["startMs"].get<std::int64_t>());
                    }
                    parsed.label = BoundedText(section, "label", MaximumTextBytes);
                    parsed.line = BoundedText(section, "line", MaximumTextBytes);
                    if (parsed.label.empty() && parsed.line.empty()) continue;
                    outAsset.metadata.sections.push_back(std::move(parsed));
                }
                // Sorted so a hand-written file with sections out of order still lands
                // its cues at the right moments.
                std::sort(
                    outAsset.metadata.sections.begin(),
                    outAsset.metadata.sections.end(),
                    [](const SongSection& first, const SongSection& second)
                    {
                        return first.startMs < second.startMs;
                    });
            }
        }
        catch (const std::exception& failure)
        {
            outError = std::string("song.json could not be read: ") + failure.what();
            return false;
        }
    }

    FindTracks(directory, outAsset.instrumentalPath, outAsset.vocalPath);
    outAsset.hasInstrumental = !outAsset.instrumentalPath.empty();
    outAsset.hasVocal = !outAsset.vocalPath.empty();
    if (!outAsset.hasInstrumental && !outAsset.hasVocal)
    {
        outError = outAsset.metadata.title +
            " has no audio: add instrumental.wav, vocal.wav, or a single .wav file.";
        return false;
    }
    outError.clear();
    return true;
}

bool SongLibrary::Resolve(
    const std::string& query,
    std::string& outSongId,
    std::string& outError) const
{
    const std::string wanted = Lower(query);
    if (wanted.empty())
    {
        outError = "Name a song to perform.";
        return false;
    }

    const std::vector<SongSummary> songs = List();
    for (const SongSummary& song : songs)
    {
        if (Lower(song.id) == wanted)
        {
            outSongId = song.id;
            outError.clear();
            return true;
        }
    }

    std::vector<const SongSummary*> matches;
    for (const SongSummary& song : songs)
    {
        if (Lower(song.id).rfind(wanted, 0) == 0 || Lower(song.title).rfind(wanted, 0) == 0)
        {
            matches.push_back(&song);
        }
    }
    if (matches.empty())
    {
        outError = "No song matches \"" + query + "\". Use /songs to see what is there.";
        return false;
    }
    if (matches.size() > 1)
    {
        // Picking one would eventually pick the wrong one silently.
        std::string names;
        for (const SongSummary* match : matches)
        {
            if (!names.empty()) names += ", ";
            names += match->id;
        }
        outError = "\"" + query + "\" matches several songs: " + names + ".";
        return false;
    }
    outSongId = matches.front()->id;
    outError.clear();
    return true;
}

} // namespace revia::performance
