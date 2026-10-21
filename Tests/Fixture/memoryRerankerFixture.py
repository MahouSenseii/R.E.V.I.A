"""Dependency-free process/HTTP adversarial fixture for the native reranker."""
import argparse
import json
import secrets
import time
from pathlib import Path
from http.server import BaseHTTPRequestHandler, HTTPServer

parser = argparse.ArgumentParser()
parser.add_argument("--model-directory")
parser.add_argument("--threads")
args = parser.parse_args()
token = secrets.token_hex(32)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        if self.headers.get("Authorization") != "Bearer " + token:
            self.send_error(401)
            return
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        # Synthetic test requests only, retained in the test's temporary directory.
        audit = Path(args.model_directory) / "reranker-requests.jsonl"
        if audit.parent.name.startswith("revia-subsystem-tests-"):
            with audit.open("a", encoding="utf-8") as output:
                output.write(json.dumps(request) + "\n")
        mode = request["query"]
        if mode == "delay" or mode.endswith(" delay"):
            time.sleep(10)
        scores = [{"index": index, "score": float(index)} for index in range(len(request["documents"]))]
        if mode == "duplicate":
            scores[-1]["index"] = 0
        elif mode == "incomplete" or mode.endswith(" incomplete"):
            scores.pop()
        elif mode == "wrong index":
            scores[-1]["index"] = 100
        elif mode == "nonfinite":
            scores[-1]["score"] = float("nan")
        elif mode == "string score":
            scores[-1]["score"] = "10"
        elif mode == "negative index":
            scores[-1]["index"] = -1
        body = json.dumps({"scores": scores}).encode()
        if mode == "oversized":
            body = b"x" * 9000
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


with HTTPServer(("127.0.0.1", 0), Handler) as server:
    print(json.dumps({"ready": True, "port": server.server_port, "token": token,
                      "device": "cpu", "model": "cross-encoder/ms-marco-MiniLM-L6-v2"}), flush=True)
    server.serve_forever()
