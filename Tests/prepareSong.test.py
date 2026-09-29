from __future__ import annotations

import http.client
import importlib.util
import json
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "Tools" / "Singing"


def load(name: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


PREPARE = load("prepare_song")
DATASET = load("build_voice_dataset")


class SongIdAndLyricsTests(unittest.TestCase):
    def test_a_title_becomes_a_folder_name_revia_accepts(self) -> None:
        self.assertEqual(PREPARE.song_id_from_title("Bright Lights!"), "bright-lights")
        self.assertEqual(PREPARE.song_id_from_title("  Café: Night / Drive  "), "café-night-drive")
        self.assertEqual(PREPARE.song_id_from_title("..."), "song")
        self.assertEqual(len(PREPARE.song_id_from_title("x" * 200)), 64)

    def test_plain_lyrics_drop_headings_and_blanks(self) -> None:
        lines = PREPARE.read_lyric_lines("[Verse 1]\nfirst line\n\n  second line \n[Chorus]\nthird\n")
        self.assertEqual(lines, ["first line", "second line", "third"])

    def test_lrc_is_written_sorted_with_tags(self) -> None:
        text = PREPARE.lrc_from_lines([(12500, "later"), (5200, "earlier")], "Bright Lights", "tool")
        self.assertEqual(text, "[ti:Bright Lights]\n[by:tool]\n[00:05.20]earlier\n[00:12.50]later\n")
        self.assertEqual(PREPARE.format_lrc_stamp(61999), "[01:01.99]")

    def test_lines_are_timed_in_order_against_transcribed_segments(self) -> None:
        segments = [
            {"start": 1.0, "end": 3.0, "text": " Walking through the city lights"},
            {"start": 4.5, "end": 6.0, "text": " nothing like the words at all"},
            {"start": 8.0, "end": 10.0, "text": " Bright lights, don't let me go"},
            {"start": 20.0, "end": 22.0, "text": " Bright lights, don't let me go"},
        ]
        timed = PREPARE.align_lines(
            ["Walking through the city lights", "Bright lights don't let me go",
             "a line whisper never heard", "Bright lights don't let me go"], segments)
        self.assertEqual([ms for ms, _ in timed], [1000, 8000, 10500, 20000])
        self.assertEqual(timed[2][1], "a line whisper never heard")

    def test_the_descriptor_carries_provenance(self) -> None:
        descriptor = PREPARE.song_descriptor("Bright Lights", "Revia", "Suno", "Revia (RVC)",
                                             "made for this channel", "bright.mp3")
        self.assertEqual(descriptor["madeWith"], "Suno")
        self.assertEqual(descriptor["voice"], "Revia (RVC)")
        self.assertEqual(descriptor["license"], "made for this channel")
        self.assertIn("bright.mp3", descriptor["notes"])
        self.assertNotIn("sections", descriptor)

    def test_the_plan_names_the_steps_that_will_run(self) -> None:
        parser = PREPARE.build_parser()
        converted = parser.parse_args(["--lyrics", "words.txt", "--voice-model", "revia.pth"])
        self.assertEqual(PREPARE.plan_steps(converted),
                         ["decode", "separate", "convert", "lyrics-align", "write"])
        kept = parser.parse_args(["--keep-original-voice", "--lyrics", "words.lrc"])
        self.assertEqual(PREPARE.plan_steps(kept), ["decode", "separate", "keep-voice", "lyrics-copy", "write"])
        report = PREPARE.check_tools(kept)
        self.assertTrue(any(line.startswith("steps") for line in report))
        self.assertEqual(PREPARE.main(["--check"]), 0)
        self.assertEqual(PREPARE.main([]), 2)


class DatasetBuilderTests(unittest.TestCase):
    def test_a_script_becomes_sentences_of_a_speakable_length(self) -> None:
        sentences = DATASET.split_sentences(
            "Hi. This is a longer sentence that she can say in one go! Short? "
            + "A very long sentence with commas, so that it has to be cut somewhere sensible, "
            + "and then it keeps going with more words, and more words after that, until it is far too long "
            + "for one clip of speech to hold comfortably.\n")
        self.assertTrue(all(12 <= len(s) <= 220 for s in sentences), sentences)
        self.assertEqual(sentences[0], "Hi. This is a longer sentence that she can say in one go!")

    def test_each_sentence_is_one_request_to_the_worker(self) -> None:
        received = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args) -> None:  # noqa: D401 - silence
                pass

            def do_POST(self) -> None:  # noqa: N802
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                received.append((self.path, self.headers.get("Authorization"), body))
                ok = "refuse" not in body["text"]
                if ok:
                    Path(body["output_path"]).write_bytes(b"RIFF")
                payload = json.dumps({"succeeded": ok, "message": "refused" if not ok else "ok"}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as root:
                out = Path(root) / "dataset"
                written = DATASET.synthesize_all(
                    ["First sentence here.", "Please refuse this one.", "Third sentence here."], out,
                    f"http://127.0.0.1:{server.server_address[1]}", "key", "/voices/ref.wav", "ref text",
                    "English", log=lambda line: None)
                self.assertEqual(written, 2)
                self.assertTrue((out / "0001.wav").is_file() and (out / "0003.wav").is_file())
                self.assertFalse((out / "0002.wav").exists())
                self.assertEqual(len(received), 3)
                path, authorization, body = received[0]
                self.assertEqual(path, "/v1/audio/speech")
                self.assertEqual(authorization, "Bearer key")
                self.assertEqual(body["reference_audio"], "/voices/ref.wav")
                self.assertEqual(body["text"], "First sentence here.")
                # A second run reuses the clips it already has.
                again = DATASET.synthesize_all(
                    ["First sentence here.", "Third sentence here."], out,
                    f"http://127.0.0.1:{server.server_address[1]}", "key", "/voices/ref.wav", "ref text",
                    "English", log=lambda line: None)
                # 0001 is reused, 0002 (this run's second sentence) is new: one request.
                self.assertEqual(again, 2)
                self.assertEqual(len(received), 4)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
