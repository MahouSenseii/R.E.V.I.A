#!/usr/bin/env python3
"""Exports Revia's own conversation archive as a chat dataset for a persona adapter.

    python export_training_set.py --archive RuntimeData/Memory/revia_conversations.db \
        --out RuntimeData/Learning/persona.jsonl [--persona persona.txt] [--max-turns 6]

What goes in: the local, private conversations Revia archived (the only ones the
archive holds -- public Discord and stream turns never enter it, and this exporter
refuses any turn that carries a public-channel marker anyway). What stays out: any
window that holds something that looks like a credential, a card number or a private
key (the whole window is dropped, never masked, so a partial secret cannot survive),
windows shorter than two turns, and exact duplicates. Every drop is counted in the
manifest written beside the dataset, with the archive's path and the dataset's SHA-256,
so a trained adapter can say exactly what it was trained on.

The dataset is ordinary chat JSONL: {"messages":[{"role":..., "content":...}, ...]}.
"""
import argparse
import hashlib
import json
import re
import sqlite3
import sys
import time

SECRET_PATTERNS = [
    ("an API key", re.compile(r"\b(?:sk-ant-|sk-proj-|sk-)[A-Za-z0-9_\-]{16,}")),
    ("an AWS access key", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("a private key block", re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----")),
    ("a bearer token", re.compile(r"\b[Bb]earer\s+[A-Za-z0-9_\-\.=]{20,}")),
    ("a password", re.compile(r"\b(?:password|passcode|passwd)\s*[:=]\s*\S{4,}", re.IGNORECASE)),
    ("a GitHub token", re.compile(r"\bgh[pousr]_[A-Za-z0-9]{20,}\b")),
]
PUBLIC_MARKERS = re.compile(r"\[(?:discord|twitch|youtube|stream|public)[^\]]*\]", re.IGNORECASE)


def luhn_ok(digits):
    total = 0
    for index, character in enumerate(reversed(digits)):
        value = int(character)
        if index % 2 == 1:
            value *= 2
            if value > 9:
                value -= 9
        total += value
    return total % 10 == 0


def looks_like_card(text):
    for match in re.finditer(r"(?:\d[ -]?){13,19}", text):
        digits = re.sub(r"[ -]", "", match.group(0))
        if 13 <= len(digits) <= 19 and luhn_ok(digits):
            return True
    return False


def sensitive(text):
    for name, pattern in SECRET_PATTERNS:
        if pattern.search(text):
            return name
    if looks_like_card(text):
        return "a payment card number"
    return None


def load_turns(archive):
    connection = sqlite3.connect(archive)
    try:
        rows = connection.execute(
            "SELECT session_id, turn_index, role, content FROM conversation_turns "
            "ORDER BY session_id, turn_index").fetchall()
    finally:
        connection.close()
    sessions = {}
    for session_id, _index, role, content in rows:
        sessions.setdefault(session_id, []).append((role, content))
    return sessions


def windows(turns, max_turns):
    """Sliding windows that end on an assistant turn and start on a user turn."""
    out = []
    for end in range(len(turns)):
        if turns[end][0] != "assistant":
            continue
        start = max(0, end - max_turns + 1)
        while start < end and turns[start][0] != "user":
            start += 1
        if start >= end:
            continue
        out.append(turns[start:end + 1])
    return out


def export(archive, out_path, persona, max_turns, max_chars, min_turns):
    sessions = load_turns(archive)
    counts = {"sessions": len(sessions), "windows": 0, "kept": 0, "dropped": {}}
    seen = set()
    examples = []

    def drop(reason):
        counts["dropped"][reason] = counts["dropped"].get(reason, 0) + 1

    for session_id, turns in sessions.items():
        for window in windows(turns, max_turns):
            counts["windows"] += 1
            if len(window) < min_turns:
                drop("too short")
                continue
            text = "\n".join(content for _role, content in window)
            if PUBLIC_MARKERS.search(text):
                drop("public channel marker")
                continue
            secret = sensitive(text)
            if secret:
                drop("held " + secret)
                continue
            if len(text) > max_chars:
                drop("too long")
                continue
            key = hashlib.sha256(text.encode("utf-8")).hexdigest()
            if key in seen:
                drop("duplicate")
                continue
            seen.add(key)
            messages = []
            if persona:
                messages.append({"role": "system", "content": persona})
            messages.extend({"role": role, "content": content} for role, content in window)
            examples.append({"messages": messages, "source_session": session_id})
            counts["kept"] += 1

    digest = hashlib.sha256()
    with open(out_path, "w", encoding="utf-8") as handle:
        for example in examples:
            line = json.dumps(example, ensure_ascii=False) + "\n"
            handle.write(line)
            digest.update(line.encode("utf-8"))
    manifest = {
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "archive": archive,
        "dataset": out_path,
        "dataset_sha256": digest.hexdigest(),
        "persona_prompt": bool(persona),
        "max_turns": max_turns,
        "max_chars": max_chars,
        "counts": counts,
        "excluded_by_design": [
            "public Discord, Twitch and YouTube turns (never archived; markers refused)",
            "windows holding credentials, tokens, private keys or card numbers (dropped whole)",
            "outputs of Claude, GPT or Gemini (advisor notes are never archived as her turns)",
        ],
    }
    with open(out_path + ".manifest.json", "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2)
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--archive", required=True, help="revia_conversations.db")
    parser.add_argument("--out", required=True, help="dataset JSONL to write")
    parser.add_argument("--persona", help="a text file used as the system prompt of every example")
    parser.add_argument("--max-turns", type=int, default=6)
    parser.add_argument("--max-chars", type=int, default=4000)
    parser.add_argument("--min-turns", type=int, default=2)
    args = parser.parse_args(argv)
    persona = None
    if args.persona:
        with open(args.persona, "r", encoding="utf-8") as handle:
            persona = handle.read().strip()
    manifest = export(args.archive, args.out, persona, args.max_turns, args.max_chars, args.min_turns)
    print(json.dumps(manifest["counts"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
