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
parser.add_argument("--variant", default="")
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
            mode = request.get("mode", "text_to_image")
            source = request.get("source", {})
            steps = min(max(1, request["steps"]), 4)
            strength = request.get("strength", 0) if mode == "reference_to_image" else 0
            while mode == "reference_to_image" and steps < 4 and int(steps * strength) < 1:
                steps += 1
            job.update(state="succeeded", path=request["outputPath"], sha256=hashlib.sha256(png).hexdigest(),
                       width=width, height=height, steps=steps, step=steps, device="cpu", deviceName="fixture CPU",
                       mode=mode, provider="local_diffusers", pipeline=("StableDiffusionImg2ImgPipeline" if mode == "reference_to_image" else "StableDiffusionPipeline"),
                       variant=args.variant, seed=request["seed"], requestedSteps=max(1, min(50, request["steps"])),
                       requestedStrength=strength, effectiveStrength=strength, totalInferenceSteps=steps,
                       denoisingSteps=int(steps * strength) if mode == "reference_to_image" else steps,
                       sourceId=source.get("id", ""), sourceDigest=source.get("sha256", ""),
                       preparedSourceDigest=hashlib.sha256(b"fixture-prepared-pixels").hexdigest() if source else "",
                       sourcePreparation="contain-pad-rgb-v1" if source else "")
            if source:
                source_path = Path(request["outputPath"]).parent / source["path"]
                if hashlib.sha256(source_path.read_bytes()).hexdigest() != source["sha256"]:
                    job.update(state="failed", error="Source bytes did not match the native snapshot")
            if request["prompt"] == "wrong source":
                job["sourceDigest"] = "0" * 64
            if request["prompt"] == "wrong mode":
                job["mode"] = "text_to_image"
            if request["prompt"] == "wrong seed":
                job["seed"] ^= 1
            if request["prompt"] == "zero denoise":
                job["denoisingSteps"] = 0
            if request["prompt"] == "wrong metadata":
                job["deviceName"] = 42
        jobs[job["jobId"]] = job
        self.reply(202, job)


ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()
