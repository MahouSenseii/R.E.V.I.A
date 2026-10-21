"""Dependency-free adversarial process/HTTP fixture; never loads a model."""
import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import secrets
import time

MODEL = "cross-encoder/nli-deberta-v3-small@fa2804872c3b4bd748f38c0185cc85775361e735"
parser = argparse.ArgumentParser()
parser.add_argument("--model-directory", type=Path, required=True)
parser.add_argument("--threads", required=True)
args = parser.parse_args()
token = secrets.token_hex(32)
mode_file = args.model_directory / "fixture-mode.txt"
startup = mode_file.read_text(encoding="utf-8") if mode_file.exists() else "normal"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        if self.path != "/compare" or self.headers.get("Authorization") != "Bearer " + token:
            self.send_error(401)
            return
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with (args.model_directory / "requests.jsonl").open("a", encoding="utf-8") as audit:
            audit.write(json.dumps(request) + "\n")
        mode = request["left"]
        if mode == "delay":
            time.sleep(10)
        result = {"model": MODEL, "truncated": False,
                  "forward": {"entailment": 0.99, "contradiction": 0.002},
                  "reverse": {"entailment": 0.98, "contradiction": 0.003}}
        if mode == "wrong model":
            result["model"] = "untrusted/model"
        elif mode == "truncated":
            result["truncated"] = True
        elif mode == "truncated type":
            result["truncated"] = "false"
        elif mode == "incomplete":
            del result["reverse"]
        elif mode == "extra":
            result["write_memory"] = True
        elif mode == "direction type":
            result["forward"] = [0.99, 0.002]
        elif mode == "string probability":
            result["forward"]["entailment"] = "0.99"
        elif mode == "boolean probability":
            result["forward"]["entailment"] = True
        elif mode == "negative":
            result["reverse"]["contradiction"] = -0.1
        elif mode == "greater than one":
            result["reverse"]["entailment"] = 1.1
        elif mode == "probability sum":
            result["reverse"]["contradiction"] = 0.5
        elif mode == "nonfinite":
            result["forward"]["entailment"] = float("nan")
        elif mode == "infinity":
            result["reverse"]["contradiction"] = float("inf")
        body = json.dumps(result).encode()
        if mode == "oversized":
            body = b"x" * 9000
        elif mode == "duplicate key":
            body = body[:-1] + b',"truncated":false}'
        elif mode == "duplicate probability":
            body = body.replace(b'"entailment": 0.99', b'"entailment": 0.1,"entailment": 0.99')
        self.send_response(503 if mode == "unavailable" else 200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


with HTTPServer(("127.0.0.1", 0), Handler) as server:
    if startup == "delay":
        time.sleep(10)
    receipt = {"ready": True, "port": server.server_port, "token": token, "device": "cpu", "model": MODEL}
    if startup == "wrapped port":
        receipt["port"] += 2 ** 32
    elif startup == "float port":
        receipt["port"] = float(server.server_port)
    elif startup == "boolean port":
        receipt["port"] = True
    elif startup == "wrong model":
        receipt["model"] = "untrusted/model"
    elif startup == "gpu":
        receipt["device"] = "cuda"
    elif startup == "token":
        receipt["token"] = "f" * 63
    print(json.dumps(receipt), flush=True)
    server.serve_forever()
