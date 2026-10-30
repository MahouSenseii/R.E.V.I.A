"""CPU-only video transport fixture. It never imports a model runtime."""

import argparse
import importlib.util
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument("--job", required=True)
arguments = parser.parse_args()
job_path = Path(arguments.job)
worker_path = Path(__file__).resolve().parents[2] / "Tools" / "revia_video_worker.py"
spec = importlib.util.spec_from_file_location("video_resource_fixture", worker_path)
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)
job = worker.load_job(job_path)
directory = job_path.parent
(Path(job["outputRoot"]) / "fixture-job-directory.txt").write_text(str(directory), encoding="utf-8")
prompt = job["prompt"]
if prompt in ("wait", "timeout", "flood"):
    child = subprocess.Popen([sys.executable, "-c", "import time;time.sleep(60)"],
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    (Path(job["outputRoot"]) / "fixture-child.pid").write_text(str(child.pid))
    if prompt == "flood":
        (directory / "result.json").write_bytes(b"x" * 100000)
    print(json.dumps({"state": "loading", "step": 0}), flush=True)
    time.sleep(60)
    raise SystemExit(1)

if job.get("source"):
    source = (directory / "source.bin").read_bytes()
    if hashlib.sha256(source).hexdigest() != job["source"]["sha256"]:
        raise ValueError("The native source snapshot did not match its digest")

output = Path(job["outputPath"])
frames = 48 if prompt == "wrong frame count" else 49
fps = 24 if prompt == "wrong timestamps" else 25
if prompt == "corrupt":
    output.write_bytes(b"not an MP4")
else:
    command = [job["ffmpegPath"], "-nostdin", "-hide_banner", "-v", "error", "-n", "-f", "lavfi", "-i",
               f"color=c=red:s=384x256:r={fps}", "-frames:v", str(frames), "-an", "-c:v", "libopenh264",
               "-threads", "1", "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(output)]
    subprocess.run(command, check=True, stdin=subprocess.DEVNULL, timeout=20,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
source = job.get("source") or {}
resources = worker.resource_snapshot()
receipt = {"path": str(output), "sha256": hashlib.sha256(output.read_bytes()).hexdigest(), "jobId": job["jobId"],
           "model": "Lightricks/LTX-Video-0.9.5", "revision": "e58e28c39631af4d1468ee57a853764e11c1d37e",
           "mode": job["mode"], "seed": job["seed"], "sourceId": source.get("id", ""), "sourceDigest": source.get("sha256", ""),
           "sourcePreparation": "contain-pad-rgb-black-v1" if source else "",
           "preparedSourceDigest": hashlib.sha256(b"fake prepared pixels").hexdigest() if source else "",
           "width": 384, "height": 256, "frameCount": 49, "fps": 25, "durationMs": 1960, "steps": 50, "denoisingSteps": 50,
           "provider": "local_diffusers_ltx", "pipeline": "LTXImageToVideoPipeline" if source else "LTXPipeline",
           "device": "cuda:1", "dtype": "float32", "offload": "sequential_cpu", "cpuThreads": 6,
           "modelLoadMilliseconds": 0, "inferenceMilliseconds": 0, "encodingMilliseconds": 1,
           "workerPid": resources["workerPid"], "peakWorkerRssMiB": resources["peakWorkerRssMiB"],
           "minimumFreeSystemRamBytes": resources["freeSystemRamBytes"], "processTreeMemoryMeasured": False}
if prompt == "wrong seed":
    receipt["seed"] ^= 1
if prompt == "wrong source":
    receipt["sourceDigest"] = "0" * 64
if prompt == "wrong device":
    receipt["device"] = "cuda:0"
(directory / "result.json").write_text(json.dumps(receipt), encoding="utf-8")
print(json.dumps({"state": "complete", "step": 50}), flush=True)
if prompt == "delayed exit":
    sys.stdout.close()
    time.sleep(0.2)
