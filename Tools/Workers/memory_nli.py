"""Optional local CPU entailment worker; it cannot read or change memory stores."""
from __future__ import annotations

import argparse
import contextlib
import hmac
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import math
import os
from pathlib import Path
import secrets
import sys

MODEL_REVISION = "fa2804872c3b4bd748f38c0185cc85775361e735"
MODEL_IDENTITY = "cross-encoder/nli-deberta-v3-small@" + MODEL_REVISION
WEIGHTS_SHA256 = "ebc79588dd73ccfb6a3f6078519cfbf512c5305384c5ea1845bc71cd32216e86"
MAX_TEXT_BYTES = 1024
MAX_REQUEST_BYTES = 16384
MAX_TOKENS = 512
LABELS = {0: "contradiction", 1: "entailment", 2: "neutral"}


class LocalServer(HTTPServer):
    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(2)
        return connection, address


def valid_text(value):
    return isinstance(value, str) and bool(value) and "\0" not in value and len(value.encode("utf-8")) <= MAX_TEXT_BYTES


def score_request(model, request):
    if not isinstance(request, dict) or set(request) != {"left", "right"}:
        raise ValueError("Invalid entailment request.")
    left, right = request["left"], request["right"]
    if not valid_text(left) or not valid_text(right):
        raise ValueError("Invalid entailment input.")
    # A shortened pair can silently remove the very negation/correction under test.
    tokens = model.tokenizer([left, right], [right, left], truncation=False, add_special_tokens=True)
    ids = tokens.get("input_ids")
    if not isinstance(ids, list) or len(ids) != 2 or any(
            not isinstance(row, list) or not 0 < len(row) <= MAX_TOKENS for row in ids):
        raise ValueError("Entailment input would be truncated.")
    scores = model.predict([(left, right), (right, left)], batch_size=2, show_progress_bar=False, apply_softmax=True)
    if len(scores) != 2:
        raise ValueError("Incomplete entailment result.")
    result = {"model": MODEL_IDENTITY, "truncated": False}
    for direction, row in zip(("forward", "reverse"), scores):
        if len(row) != 3:
            raise ValueError("Invalid entailment label count.")
        values = [float(value) for value in row]
        if any(not math.isfinite(value) or not 0 <= value <= 1 for value in values) or abs(sum(values) - 1) > 0.0001:
            raise ValueError("Invalid entailment probabilities.")
        result[direction] = {"entailment": values[1], "contradiction": values[0]}
    return result


def verify_model_files(directory: Path):
    manifest = json.loads((directory / "installation.json").read_text(encoding="utf-8"))
    if manifest.get("revision") != MODEL_REVISION or manifest.get("model") != "cross-encoder/nli-deberta-v3-small":
        raise ValueError("Installed entailment model identity is not pinned.")
    allowed = {"config.json", "tokenizer.json", "tokenizer_config.json", "special_tokens_map.json",
               "added_tokens.json", "spm.model", "model.safetensors"}
    seen = set()
    for record in manifest.get("files", []):
        name = record["file"]
        if name not in allowed or name in seen:
            raise ValueError("Invalid entailment installation manifest.")
        seen.add(name)
        path = directory / name
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for block in iter(lambda: source.read(4 * 1024 * 1024), b""):
                digest.update(block)
        if digest.hexdigest() != record["sha256"] or path.stat().st_size != record["bytes"]:
            raise ValueError("Installed entailment files changed.")
        if name == "model.safetensors" and digest.hexdigest() != WEIGHTS_SHA256:
            raise ValueError("Entailment weights do not match the pinned revision.")
    if not {"model.safetensors", "config.json", "tokenizer.json", "tokenizer_config.json"}.issubset(seen):
        raise ValueError("Incomplete entailment installation.")
    if any((directory / name).exists() and name not in seen for name in allowed):
        raise ValueError("An installed tokenizer artifact is absent from the provenance manifest.")
    return manifest


def load_model(directory: Path, threads: int):
    if not directory.is_absolute() or not directory.is_dir() or not 1 <= threads <= 8:
        raise ValueError("Entailment requires an installed local model and 1-8 CPU threads.")
    verify_model_files(directory)
    os.environ.update(HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1", HF_HUB_DISABLE_TELEMETRY="1",
                      CUDA_VISIBLE_DEVICES="", TOKENIZERS_PARALLELISM="false")
    import torch
    from sentence_transformers import CrossEncoder

    torch.set_num_threads(threads)
    torch.set_num_interop_threads(1)
    model = CrossEncoder(str(directory), device="cpu", local_files_only=True, trust_remote_code=False,
                         model_kwargs={"use_safetensors": True}, max_length=MAX_TOKENS)
    if model.model.config.id2label != LABELS:
        raise ValueError("The entailment label mapping does not match the pinned model.")
    return model


def make_handler(model, token):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, *_):
            pass

        def reply(self, code, body):
            data = json.dumps(body, allow_nan=False, separators=(",", ":")).encode("utf-8")
            try:
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            except ConnectionError:
                pass

        def discard_bounded_body(self):
            # Windows can reset a connection closed with unread request bytes,
            # hiding the 401. Never interpret this input or drain unbounded data.
            if self.headers.get("Transfer-Encoding"):
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if 0 < length <= MAX_REQUEST_BYTES:
                    self.rfile.read(length)
            except (ValueError, OSError):
                pass

        def do_POST(self):
            if not hmac.compare_digest(self.headers.get("Authorization", ""), "Bearer " + token):
                self.discard_bounded_body()
                return self.reply(401, {"error": "Unauthorized."})
            if self.path != "/compare" or self.headers.get("Transfer-Encoding"):
                return self.reply(400, {"error": "Invalid entailment request."})
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= MAX_REQUEST_BYTES:
                    raise ValueError("Request size.")
                body = self.rfile.read(length)
                if len(body) != length:
                    raise ValueError("Truncated request.")
                result = score_request(model, json.loads(body))
                return self.reply(200, result)
            except (ValueError, TypeError, UnicodeError):
                return self.reply(400, {"error": "Invalid entailment input or scores."})
            except Exception:
                return self.reply(503, {"error": "Entailment inference failed."})
    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-directory", type=Path, required=True)
    parser.add_argument("--threads", type=int, default=2)
    args = parser.parse_args()
    try:
        with contextlib.redirect_stdout(sys.stderr):
            model = load_model(args.model_directory, args.threads)
            score_request(model, {"left": "The user enjoys tea.", "right": "The user likes tea."})
        token = secrets.token_hex(32)
        with LocalServer(("127.0.0.1", 0), make_handler(model, token)) as server:
            print(json.dumps({"ready": True, "port": server.server_port, "token": token,
                              "device": "cpu", "model": MODEL_IDENTITY}), flush=True)
            server.serve_forever(poll_interval=0.1)
    except Exception:
        print("CPU entailment worker could not load the installed local model.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
