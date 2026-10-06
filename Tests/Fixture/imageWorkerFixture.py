"""Dependency-free HTTP fixture for native image transport/lifetime checks."""
import argparse
import hashlib
import json
import struct
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--host")
parser.add_argument("--port", type=int)
parser.add_argument("--model")
parser.add_argument("--api-key")
args, _ = parser.parse_known_args()
jobs = {}


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, code, data):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            return self.reply(200, {"ok": True, "loaded": False})
        self.reply(200, jobs[self.path.removeprefix("/jobs/")])

    def do_POST(self):
        if self.headers.get("Authorization") != "Bearer " + args.api_key:
            return self.reply(401, {"ok": False})
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        job = {"ok": True, "jobId": request["jobId"], "model": args.model, "state": "loading"}
        if request["prompt"] != "wait":
            width, height = request["width"], request["height"]
            png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            png += chunk(b"IDAT", zlib.compress((b"\0" + b"\xff\0\0" * width) * height)) + chunk(b"IEND", b"")
            if request["prompt"] == "corrupt":
                png = b"not an image"
            Path(request["outputPath"]).write_bytes(png)
            job.update(state="succeeded", path=request["outputPath"], sha256=hashlib.sha256(png).hexdigest(),
                       width=width, height=height, steps=1, step=1, device="cpu", deviceName="fixture CPU")
            if request["prompt"] == "wrong metadata":
                job["deviceName"] = 42
        jobs[job["jobId"]] = job
        self.reply(202, job)


ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()
