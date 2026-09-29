#!/usr/bin/env python3
"""Synthesize a training set of Revia's own voice for an RVC model.

    python build_voice_dataset.py --script lines.txt --out dataset \
        --worker-url http://127.0.0.1:8093 --token dataset \
        --reference-audio C:/.../Voices/revia-bright/reference.wav \
        --reference-text "the transcript of that clip"

Each sentence in the script becomes one WAV from a Qwen3-TTS worker, named 0001.wav,
0002.wav ... in `--out`. Revia's own worker takes a key that exists only for her session,
so start one for the build: `qwen_tts_service.py --port 8093 --token dataset`. Ten to twenty minutes
of varied sentences is the usual amount for an RVC voice; the script is yours to write,
and everything it says is said in her voice, so keep it to things she would say.

Training itself is RVC's job (Applio or the RVC WebUI: point it at `--out`, train, and
export the .pth and .index for prepare_song.py --voice-model / --voice-index).
"""
from __future__ import annotations

import argparse
import http.client
import json
import re
import sys
import urllib.parse
from pathlib import Path
from typing import Callable


def split_sentences(text: str, minimum_characters: int = 12, maximum_characters: int = 220) -> list[str]:
    """One sentence per line for the worker: short ones merged, long ones split at commas."""
    pieces = [piece.strip() for piece in re.split(r"(?<=[.!?])\s+|\n+", text) if piece.strip()]
    sentences: list[str] = []
    carry = ""
    for piece in pieces:
        candidate = f"{carry} {piece}".strip() if carry else piece
        if len(candidate) < minimum_characters:
            carry = candidate
            continue
        carry = ""
        while len(candidate) > maximum_characters:
            cut = candidate.rfind(",", 0, maximum_characters)
            if cut < minimum_characters:
                cut = maximum_characters
            sentences.append(candidate[:cut + 1].strip() if candidate[cut] == "," else candidate[:cut].strip())
            candidate = candidate[cut + 1:].strip()
        if candidate:
            sentences.append(candidate)
    if carry:
        sentences.append(carry)
    return sentences


def speech_request(text: str, reference_audio: str, reference_text: str, language: str, output_path: str) -> dict:
    return {"text": text, "language": language, "reference_audio": reference_audio,
            "reference_text": reference_text, "output_path": output_path}


def synthesize_all(sentences: list[str], out: Path, worker_url: str, token: str,
                   reference_audio: str, reference_text: str, language: str,
                   log: Callable[[str], None] = print) -> int:
    parsed = urllib.parse.urlparse(worker_url)
    out.mkdir(parents=True, exist_ok=True)
    written = 0
    for index, sentence in enumerate(sentences, start=1):
        target = out / f"{index:04d}.wav"
        if target.is_file():
            written += 1
            continue
        body = json.dumps(speech_request(sentence, reference_audio, reference_text, language, str(target)))
        connection = http.client.HTTPConnection(parsed.hostname or "127.0.0.1", parsed.port or 8092, timeout=600)
        connection.request("POST", "/v1/audio/speech", body,
                           {"Authorization": f"Bearer {token}", "Content-Type": "application/json"})
        response = connection.getresponse()
        payload = json.loads(response.read().decode("utf-8") or "{}")
        connection.close()
        if response.status != 200 or not payload.get("succeeded", False):
            log(f"{index:04d}: refused ({payload.get('message', response.status)})")
            continue
        written += 1
        log(f"{index:04d}: {sentence[:60]}")
    return written


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--script", required=True, help="a text file of sentences for her to say")
    parser.add_argument("--out", required=True)
    parser.add_argument("--reference-audio", required=True, help="the voice preset's reference.wav")
    parser.add_argument("--reference-text", required=True, help="that clip's transcript")
    parser.add_argument("--language", default="English")
    parser.add_argument("--worker-url", default="http://127.0.0.1:8092")
    parser.add_argument("--token", default="")
    parser.add_argument("--token-file", default="", help="the file Revia keeps her local API key in")
    args = parser.parse_args(argv)
    token = args.token or (Path(args.token_file).read_text(encoding="utf-8").strip() if args.token_file else "")
    if not token:
        print("Pass --token or --token-file; the worker refuses requests without Revia's local key.", file=sys.stderr)
        return 2
    sentences = split_sentences(Path(args.script).read_text(encoding="utf-8"))
    written = synthesize_all(sentences, Path(args.out), args.worker_url, token,
                             str(Path(args.reference_audio).resolve()), args.reference_text, args.language)
    print(f"{written} of {len(sentences)} clips in {args.out}")
    return 0 if written == len(sentences) else 1


if __name__ == "__main__":
    sys.exit(main())
