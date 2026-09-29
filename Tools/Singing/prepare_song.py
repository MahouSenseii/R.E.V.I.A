#!/usr/bin/env python3
"""Turn a song made with Suno (or any recording you have the rights to) into a song
Revia can perform in her own voice.

    python prepare_song.py --input "Bright Lights.mp3" --title "Bright Lights" \
        --songs-root C:/path/to/Revia/build/debug/RuntimeData/Songs \
        --lyrics "Bright Lights.txt" --voice-model revia.pth --voice-index revia.index

Steps, each reported as it runs:

  1. decode    the input to a WAV (soundfile; ffmpeg when soundfile cannot read it)
  2. separate  the vocal from the instrumental (python-audio-separator, RoFormer)
  3. convert   the vocal into Revia's voice (RVC, a model trained on her own voice;
               --keep-original-voice skips this and says so in the descriptor)
  4. lyrics    copy an .lrc as it is, or time a plain .txt against the vocal with the
               local whisper server so the karaoke lines follow the song
  5. write     RuntimeData/Songs/<id>/{instrumental.wav, vocal.wav, lyrics.lrc, song.json}

The descriptor records where the recording came from ("madeWith": "Suno") and whose
voice sings it, and Revia announces both when she performs it. Nothing here generates
music: Suno makes the song, this makes it hers to sing.

--check reports which tools are installed without touching any audio. The heavy steps
are imported only when they run, so the pure parts (ids, lyrics timing, the descriptor)
are testable without a GPU or the audio stack.
"""
from __future__ import annotations

import argparse
import difflib
import json
import re
import shutil
import subprocess
import sys
import unicodedata
from pathlib import Path
from typing import Any, Callable, Iterable

DEFAULT_SEPARATOR_MODEL = "model_bs_roformer_ep_317_sdr_12.9755.ckpt"


# ----------------------------------------------------------------------------- pure parts

def song_id_from_title(title: str) -> str:
    """A folder name Revia accepts: letters, digits and dashes, no dots or slashes."""
    text = unicodedata.normalize("NFKC", title).strip().lower()
    text = re.sub(r"[^\w\s-]", "", text, flags=re.UNICODE)
    text = re.sub(r"[\s_-]+", "-", text).strip("-")
    return text[:64] or "song"


def read_lyric_lines(text: str) -> list[str]:
    """Plain lyrics: one line per line, blanks and [Verse]-style headings dropped."""
    lines = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or (line.startswith("[") and line.endswith("]")):
            continue
        lines.append(line)
    return lines


def format_lrc_stamp(milliseconds: int) -> str:
    milliseconds = max(0, int(milliseconds))
    minutes, rest = divmod(milliseconds, 60000)
    seconds, hundredths = divmod(rest, 1000)
    return f"[{minutes:02d}:{seconds:02d}.{hundredths // 10:02d}]"


def lrc_from_lines(timed: Iterable[tuple[int, str]], title: str = "", by: str = "") -> str:
    """LRC text, sorted by time, with the tags Revia's reader skips."""
    header = []
    if title:
        header.append(f"[ti:{title}]")
    if by:
        header.append(f"[by:{by}]")
    body = [f"{format_lrc_stamp(ms)}{line}" for ms, line in sorted(timed, key=lambda pair: pair[0])]
    return "\n".join(header + body) + "\n"


def _normalise_words(text: str) -> str:
    return re.sub(r"[^a-z0-9 ]", "", text.lower()).strip()


def align_lines(lyric_lines: list[str], segments: list[dict[str, Any]]) -> list[tuple[int, str]]:
    """Give each lyric line the start time of the transcribed segment it matches.

    Greedy and in order: a lyric never jumps back before the segment its predecessor
    took, which is what keeps a repeated chorus on its second occurrence rather than
    its first. A line with no plausible match takes the time of the previous line plus
    a beat, so every line is shown at some point rather than dropped. `segments` are
    {"start": seconds, "end": seconds, "text": ...} as whisper reports them.
    """
    timed: list[tuple[int, str]] = []
    cursor = 0
    last_ms = 0
    for line in lyric_lines:
        target = _normalise_words(line)
        best_index, best_score = -1, 0.0
        for index in range(cursor, len(segments)):
            candidate = _normalise_words(str(segments[index].get("text", "")))
            if not candidate:
                continue
            score = difflib.SequenceMatcher(None, target, candidate).ratio()
            if score > best_score:
                best_index, best_score = index, score
            if score > 0.9:
                break
        if best_index >= 0 and best_score >= 0.45:
            start_ms = int(float(segments[best_index].get("start", 0.0)) * 1000)
            cursor = best_index + 1
            last_ms = max(start_ms, last_ms)
        else:
            last_ms = last_ms + 2500
            start_ms = last_ms
        timed.append((start_ms, line))
    return timed


def song_descriptor(
    title: str,
    artist: str,
    made_with: str,
    voice: str,
    license_text: str,
    source_name: str,
    instrumental_gain: float = 0.9,
    vocal_gain: float = 1.0,
) -> dict[str, Any]:
    """The song.json Revia reads: title, credit, provenance, gains. Sections are left to
    lyrics.lrc so a hand edit of either file does not fight the other."""
    return {
        "title": title,
        "artist": artist,
        "madeWith": made_with,
        "voice": voice,
        "license": license_text,
        "notes": f"Prepared by Tools/Singing/prepare_song.py from {source_name}.",
        "instrumentalGain": instrumental_gain,
        "vocalGain": vocal_gain,
    }


def plan_steps(args: argparse.Namespace) -> list[str]:
    """Which steps this invocation will run, for --check and the log."""
    steps = ["decode", "separate"]
    if args.keep_original_voice:
        steps.append("keep-voice")
    else:
        steps.append("convert")
    if args.lyrics:
        steps.append("lyrics-copy" if Path(args.lyrics).suffix.lower() == ".lrc" else "lyrics-align")
    steps.append("write")
    return steps


# --------------------------------------------------------------------------- heavy parts

def decode_to_wav(source: Path, target: Path, log: Callable[[str], None]) -> None:
    """A 16-bit WAV of the input, through soundfile when it can read the format and
    ffmpeg otherwise."""
    try:
        import soundfile as sf

        data, rate = sf.read(str(source), always_2d=True)
        sf.write(str(target), data, rate, subtype="PCM_16")
        log(f"decoded {source.name} with soundfile ({rate} Hz, {data.shape[1]} ch)")
        return
    except Exception as error:  # noqa: BLE001 - reported, then ffmpeg is tried
        log(f"soundfile could not read {source.name} ({error}); trying ffmpeg")
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("Neither soundfile nor ffmpeg could decode the input. Export the "
                           "song from Suno as WAV, or install ffmpeg.")
    subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", str(source),
                    "-acodec", "pcm_s16le", str(target)], check=True)
    log(f"decoded {source.name} with ffmpeg")


def separate(wav: Path, work: Path, model_name: str, log: Callable[[str], None]) -> tuple[Path, Path]:
    """Vocal and instrumental WAVs from one mix, via python-audio-separator."""
    from audio_separator.separator import Separator

    work.mkdir(parents=True, exist_ok=True)
    separator = Separator(output_dir=str(work), output_format="WAV", log_level=30)
    separator.load_model(model_filename=model_name)
    outputs = [work / name if not Path(name).is_absolute() else Path(name)
               for name in separator.separate(str(wav))]
    vocal = next((path for path in outputs if "vocal" in path.name.lower()), None)
    instrumental = next((path for path in outputs if path is not vocal), None)
    if vocal is None or instrumental is None:
        raise RuntimeError(f"The separator wrote {[p.name for p in outputs]}, not a vocal and an instrumental.")
    log(f"separated into {vocal.name} and {instrumental.name}")
    return vocal, instrumental


def convert_voice(vocal: Path, target: Path, model: Path, index: Path | None,
                  device: str, pitch: int, log: Callable[[str], None]) -> None:
    """The vocal in Revia's voice, through RVC (rvc-python)."""
    from rvc_python.infer import RVCInference

    rvc = RVCInference(device=device)
    rvc.load_model(str(model), index_path=str(index) if index else None)
    if pitch:
        rvc.set_params(f0up_key=pitch)
    rvc.infer_file(str(vocal), str(target))
    log(f"converted the vocal with {model.name}" + (f" (pitch {pitch:+d})" if pitch else ""))


def transcribe_segments(vocal: Path, whisper_url: str, log: Callable[[str], None]) -> list[dict[str, Any]]:
    """Timed segments from the local whisper server, for lining lyrics up."""
    import http.client
    import urllib.parse
    import uuid

    parsed = urllib.parse.urlparse(whisper_url)
    boundary = uuid.uuid4().hex
    body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\nverbose_json\r\n"
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"vocal.wav\"\r\n"
            f"Content-Type: audio/wav\r\n\r\n").encode() + vocal.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
    connection = http.client.HTTPConnection(parsed.hostname or "127.0.0.1", parsed.port or 8094, timeout=600)
    connection.request("POST", "/inference", body,
                       {"Content-Type": f"multipart/form-data; boundary={boundary}"})
    response = connection.getresponse()
    payload = json.loads(response.read().decode("utf-8"))
    if response.status != 200:
        raise RuntimeError(f"whisper answered {response.status}: {payload}")
    segments = payload.get("segments") or []
    log(f"whisper found {len(segments)} segments")
    return segments


def check_tools(args: argparse.Namespace) -> list[str]:
    """What is installed, one line each; nothing is run."""
    report = []
    for module, purpose in (("soundfile", "decode"), ("audio_separator", "separate"), ("rvc_python", "convert")):
        try:
            __import__(module)
            report.append(f"ok       {module} ({purpose})")
        except Exception as error:  # noqa: BLE001
            report.append(f"missing  {module} ({purpose}): {error}")
    report.append(f"{'ok      ' if shutil.which('ffmpeg') else 'missing '} ffmpeg (decode fallback)")
    if args.voice_model:
        report.append(f"{'ok      ' if Path(args.voice_model).is_file() else 'missing '} voice model {args.voice_model}")
    report.append("steps    " + " -> ".join(plan_steps(args)))
    return report


def prepare(args: argparse.Namespace, log: Callable[[str], None] = print) -> Path:
    source = Path(args.input).expanduser().resolve()
    if not source.is_file():
        raise FileNotFoundError(f"No such input: {source}")
    songs_root = Path(args.songs_root).expanduser().resolve()
    song_id = args.id or song_id_from_title(args.title)
    if not re.fullmatch(r"[\w-]{1,64}", song_id) or song_id.startswith("-"):
        raise ValueError(f"'{song_id}' is not a usable song id.")
    folder = songs_root / song_id
    if folder.exists() and not args.overwrite:
        raise FileExistsError(f"{folder} already exists; pass --overwrite to replace it.")
    work = folder / ".work"
    work.mkdir(parents=True, exist_ok=True)

    decoded = work / "source.wav"
    decode_to_wav(source, decoded, log)
    vocal, instrumental = separate(decoded, work, args.separator_model, log)
    shutil.copyfile(instrumental, folder / "instrumental.wav")

    voice = "original"
    if args.keep_original_voice:
        shutil.copyfile(vocal, folder / "vocal.wav")
        log("kept the original singer's voice (--keep-original-voice)")
    else:
        if not args.voice_model:
            raise ValueError("Pass --voice-model (an RVC model of Revia's voice) or --keep-original-voice.")
        convert_voice(vocal, folder / "vocal.wav", Path(args.voice_model),
                      Path(args.voice_index) if args.voice_index else None,
                      args.device, args.pitch, log)
        voice = args.voice_name

    if args.lyrics:
        lyrics = Path(args.lyrics).expanduser()
        if lyrics.suffix.lower() == ".lrc":
            shutil.copyfile(lyrics, folder / "lyrics.lrc")
            log("copied the timed lyrics")
        else:
            lines = read_lyric_lines(lyrics.read_text(encoding="utf-8"))
            segments = transcribe_segments(vocal, args.whisper_url, log)
            timed = align_lines(lines, segments)
            (folder / "lyrics.lrc").write_text(lrc_from_lines(timed, args.title, "prepare_song.py"), encoding="utf-8")
            log(f"timed {len(timed)} lyric lines against the vocal")

    descriptor = song_descriptor(args.title, args.artist, args.made_with, voice, args.license,
                                 source.name, args.instrumental_gain, args.vocal_gain)
    (folder / "song.json").write_text(json.dumps(descriptor, indent=2, ensure_ascii=False) + "\n",
                                      encoding="utf-8")
    if not args.keep_work:
        shutil.rmtree(work, ignore_errors=True)
    log(f"ready: {folder}  (/sing {song_id})")
    return folder


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", help="the song as Suno exported it (mp3 or wav)")
    parser.add_argument("--title", default="")
    parser.add_argument("--artist", default="Revia")
    parser.add_argument("--id", default="", help="folder name; derived from the title when omitted")
    parser.add_argument("--songs-root", default="", help="Revia's RuntimeData/Songs folder")
    parser.add_argument("--lyrics", default="", help="lyrics as .lrc (copied) or .txt (timed against the vocal)")
    parser.add_argument("--made-with", default="Suno")
    parser.add_argument("--license", default="Made for this channel with Suno; performed by Revia.")
    parser.add_argument("--voice-model", default="", help="RVC .pth trained on Revia's voice")
    parser.add_argument("--voice-index", default="", help="the matching RVC .index file")
    parser.add_argument("--voice-name", default="Revia (RVC)")
    parser.add_argument("--keep-original-voice", action="store_true")
    parser.add_argument("--pitch", type=int, default=0, help="RVC pitch shift in semitones")
    parser.add_argument("--device", default="cuda:0")
    parser.add_argument("--separator-model", default=DEFAULT_SEPARATOR_MODEL)
    parser.add_argument("--whisper-url", default="http://127.0.0.1:8094")
    parser.add_argument("--instrumental-gain", type=float, default=0.9)
    parser.add_argument("--vocal-gain", type=float, default=1.0)
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("--keep-work", action="store_true")
    parser.add_argument("--check", action="store_true", help="report installed tools and the plan; run nothing")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.check:
        print("\n".join(check_tools(args)))
        return 0
    if not args.input or not args.songs_root or not args.title:
        print("--input, --title and --songs-root are required (or --check).", file=sys.stderr)
        return 2
    try:
        prepare(args)
    except Exception as error:  # noqa: BLE001 - one line for the person at the keyboard
        print(f"prepare_song: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
