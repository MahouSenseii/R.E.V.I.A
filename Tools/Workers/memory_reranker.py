"""Owned CPU reranker. The runtime only loads local files; setup is separate."""
from __future__ import annotations

import argparse
import contextlib
import hmac
import json
import math
import os
from pathlib import Path
import secrets
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

MAX_CANDIDATES = 30
MAX_QUERY_BYTES = 4096
MAX_DOCUMENT_BYTES = 8192
MAX_REQUEST_BYTES = 300_000
REFERENCE_MODEL = "cross-encoder/ms-marco-MiniLM-L6-v2"


class LocalServer(HTTPServer):
    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(2)
        return connection, address


def valid_text(value, maximum):
    return isinstance(value, str) and bool(value) and "\0" not in value and len(value.encode("utf-8")) <= maximum


def score_request(model, request):
    if not isinstance(request, dict) or set(request) != {"query", "documents"}:
        raise ValueError("Invalid reranker request.")
    query, documents = request["query"], request["documents"]
    if not valid_text(query, MAX_QUERY_BYTES) or not isinstance(documents, list) or not 1 <= len(documents) <= MAX_CANDIDATES:
        raise ValueError("Invalid reranker input bounds.")
    if any(not valid_text(text, MAX_DOCUMENT_BYTES) for text in documents):
        raise ValueError("Invalid reranker document.")
    # Preserve pair positions. The native caller checks a complete permutation and
    # sorts only its admitted shortlist, never rows supplied by this worker.
    scores = model.predict([(query, document) for document in documents], batch_size=16, show_progress_bar=False)
    if len(scores) != len(documents):
        raise ValueError("Incomplete reranker scores.")
    result = []
    for index, score in enumerate(scores):
        if isinstance(score, (list, tuple)) or getattr(score, "ndim", 0) != 0:
            raise ValueError("Expected scalar reranker scores.")
        value = float(score)
        if not math.isfinite(value):
            raise ValueError("Nonfinite reranker scores.")
        result.append({"index": index, "score": value})
    return {"scores": result}


def load_model(directory: Path, threads: int):
    if not directory.is_absolute() or not directory.is_dir() or not 1 <= threads <= 16:
        raise ValueError("Reranking requires an installed local model directory and 1-16 CPU threads.")
    os.environ.update(HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1", HF_HUB_DISABLE_TELEMETRY="1",
                      CUDA_VISIBLE_DEVICES="", TOKENIZERS_PARALLELISM="false")
    import torch
    from sentence_transformers import CrossEncoder

    torch.set_num_threads(threads)
    torch.set_num_interop_threads(1)
    return CrossEncoder(str(directory), device="cpu", local_files_only=True, trust_remote_code=False,
                        model_kwargs={"use_safetensors": True}, max_length=512)


def make_handler(model, token):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, *_):
            pass

        def reply(self, code, body):
            data = json.dumps(body, allow_nan=False, separators=(",", ":")).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_POST(self):
            self.connection.settimeout(2)
            if not hmac.compare_digest(self.headers.get("Authorization", ""), "Bearer " + token):
                return self.reply(401, {"error": "Unauthorized."})
            if self.path != "/rerank" or self.headers.get("Transfer-Encoding"):
                return self.reply(400, {"error": "Invalid reranker request."})
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= MAX_REQUEST_BYTES:
                    raise ValueError("Request size.")
                body = self.rfile.read(length)
                if len(body) != length:
                    raise ValueError("Truncated request.")
                request = json.loads(body)
                result = score_request(model, request)
                return self.reply(200, result)
            except (ValueError, TypeError, UnicodeError):
                return self.reply(400, {"error": "Invalid reranker input or scores."})
            except Exception:
                # Neither model diagnostics nor private query text leave the worker.
                return self.reply(503, {"error": "Reranker inference failed."})
    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-directory", type=Path, required=True)
    parser.add_argument("--threads", type=int, default=4)
    args = parser.parse_args()
    try:
        # stdout is the private startup receipt; dependency progress belongs on stderr.
        with contextlib.redirect_stdout(sys.stderr):
            model = load_model(args.model_directory, args.threads)
            score_request(model, {"query": "What did we discuss during our last conversation?",
                                 "documents": ["We discussed the project, plans for next week, and a remembered conversation."] * MAX_CANDIDATES})
        token = secrets.token_hex(32)
        with LocalServer(("127.0.0.1", 0), make_handler(model, token)) as server:
            print(json.dumps({"ready": True, "port": server.server_port, "token": token,
                              "device": "cpu", "model": REFERENCE_MODEL}), flush=True)
            server.serve_forever(poll_interval=0.1)
    except Exception:
        print("CPU memory reranker could not load the installed local model.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
