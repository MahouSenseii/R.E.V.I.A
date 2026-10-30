"""One local LTX job. The native owner controls its process tree and deadline."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import time


MODEL = "Lightricks/LTX-Video-0.9.5"
REVISION = "e58e28c39631af4d1468ee57a853764e11c1d37e"
INSTALL_MANIFEST = "f5302ae0bbfde192571ec16d3c6b31902c9ca3e290bdc132b48fdca482f13b4a"
DOWNLOAD_RECEIPT = "864c81cb93b4484ff1a2bb43581f2080a46f7d26c6ce3405674cbd78a2e86cf2"
PREFLIGHT = "b8db0bcf46124d9e724c97573934789b783210f5a276de68d4c611e7186779af"
OVERLAY_MANIFEST = "ff6f941c9580f188ad1f0ed0b8bb92610b87da38b33aa1d36b78bfa360878c57"


def resource_snapshot():
    require(os.name == "nt", "The qualified video resource guard requires Windows.")

    class MemoryStatus(ctypes.Structure):
        _fields_ = [("length", wintypes.DWORD), ("load", wintypes.DWORD)] + [
            (name, ctypes.c_ulonglong) for name in ("totalPhysical", "freePhysical", "totalPage", "freePage", "totalVirtual", "freeVirtual", "freeExtended")]

    class ProcessMemory(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in ("peakWorkingSet", "workingSet", "peakPaged", "paged", "peakNonPaged", "nonPaged", "pagefile", "peakPagefile")]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.GlobalMemoryStatusEx.argtypes = [ctypes.POINTER(MemoryStatus)]
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessMemory), wintypes.DWORD]
    status = MemoryStatus()
    status.length = ctypes.sizeof(status)
    memory = ProcessMemory()
    memory.cb = ctypes.sizeof(memory)
    require(kernel.GlobalMemoryStatusEx(ctypes.byref(status)) and
            psapi.GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(memory), memory.cb), "Video resource measurements are unavailable.")
    return {"workerPid": os.getpid(), "peakWorkerRssMiB": memory.peakWorkingSet / 1024**2,
            "freeSystemRamBytes": status.freePhysical, "processTreeMemoryMeasured": False}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def plain(path):
    path = Path(path)
    require(not str(path).startswith(("\\\\", "//")), "Network media paths are not supported.")
    for part in (path, *path.parents):
        information = part.lstat()
        require(not stat.S_ISLNK(information.st_mode) and not getattr(information, "st_file_attributes", 0) & 0x400,
                "Links and reparse points are not video inputs or destinations.")
    return path


def read_json(path, limit=65536):
    plain(path)
    with Path(path).open("rb") as stream:
        data = stream.read(limit + 1)
    require(0 < len(data) <= limit, "The local video manifest exceeds its bound.")
    value = json.loads(data)
    require(isinstance(value, dict), "A video manifest must be a JSON object.")
    return value


def validate_request(job):
    require(job.get("mode") in ("text_to_video", "image_to_video"), "Unsupported video request mode.")
    prompt = job.get("prompt")
    require(isinstance(prompt, str) and prompt.strip() and len(prompt.encode("utf-8")) <= 8192, "A bounded nonempty prompt is required.")
    require(type(job.get("seed")) is int and 0 <= job["seed"] <= 0xffffffff, "A uint32 seed is required.")
    for key, expected in {"width": 384, "height": 256, "frameCount": 49, "fps": 25, "steps": 50}.items():
        require(type(job.get(key)) is int and job[key] == expected, "Only the qualified LTX dimensions and sampling settings are supported.")
    if job["mode"] == "image_to_video":
        require(isinstance(job.get("source"), dict), "Image-to-video requires one typed reference.")
    else:
        require(job.get("source") is None, "Text-to-video cannot discard a reference.")


def load_job(path):
    path = plain(Path(path).absolute())
    job = read_json(path, 32768)
    validate_request(job)
    identifier = job.get("jobId")
    require(isinstance(identifier, str) and re.fullmatch(r"[A-Za-z0-9_-]{1,96}", identifier), "Invalid video job identity.")
    root = plain(Path(job["outputRoot"]))
    scratch = plain(Path(job["scratchRoot"]))
    directory = scratch / identifier
    require(root.is_absolute() and scratch.is_absolute() and scratch.name == "ReviaVideoJobs" and
            path == directory / "job.json" and job.get("jobDirectory") == str(directory) and
            not scratch.is_relative_to(root) and not root.is_relative_to(scratch),
            "The video job escaped its separate owned scratch directory.")
    output = root / f"video-{identifier}.mp4"
    require(job.get("outputPath") == str(output) and not output.exists(), "Invalid or existing video output destination.")
    job["directory"] = str(path.parent)
    return job


def publish_output(pending, output, expected_sha):
    pending, output = plain(Path(pending)), Path(output)
    plain(output.parent)
    before = pending.stat()
    require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1 and 0 < before.st_size <= 64 * 1024 * 1024,
            "The encoded video exceeds its plain bounded file contract.")
    created = None
    complete = False
    try:
        with pending.open("rb") as source:
            opened = os.fstat(source.fileno())
            require((opened.st_dev, opened.st_ino, opened.st_size) == (before.st_dev, before.st_ino, before.st_size) and
                    stat.S_ISREG(opened.st_mode) and opened.st_nlink == 1, "The encoded video changed before publication.")
            with output.open("xb") as destination:
                created = os.fstat(destination.fileno())
                total, value = 0, hashlib.sha256()
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    total += len(block)
                    require(total <= before.st_size, "The encoded video grew during publication.")
                    destination.write(block)
                    value.update(block)
                require(total == before.st_size and value.hexdigest() == expected_sha and
                        os.fstat(source.fileno()).st_size == before.st_size, "The encoded video changed during publication.")
                destination.flush()
                os.fsync(destination.fileno())
            complete = True
    finally:
        if created is not None and not complete:
            try:
                current = output.lstat()
                if stat.S_ISREG(current.st_mode) and (current.st_dev, current.st_ino) == (created.st_dev, created.st_ino):
                    output.unlink()
            except FileNotFoundError:
                pass


def prepare_source(job):
    from PIL import Image, ImageOps

    source = job["source"]
    require(source.get("path") == "source.bin", "Only this job's fixed source snapshot is allowed.")
    path = plain(Path(job["directory"]) / "source.bin")
    require(path.is_file() and 0 < path.stat().st_size <= 8 * 1024 * 1024, "Invalid reference byte bound.")
    require(isinstance(source.get("id"), str) and 0 < len(source["id"]) <= 128 and "\0" not in source["id"], "Invalid reference identity.")
    require(re.fullmatch(r"[0-9a-f]{64}", source.get("sha256", "")), "Invalid reference digest.")
    with path.open("rb") as stream:
        data = stream.read(8 * 1024 * 1024 + 1)
    require(len(data) <= 8 * 1024 * 1024 and hashlib.sha256(data).hexdigest() == source["sha256"], "Reference bytes changed.")
    with Image.open(io.BytesIO(data)) as opened:
        require({"PNG": "image/png", "JPEG": "image/jpeg"}.get(opened.format) == source.get("mimeType") and
                opened.format in ("PNG", "JPEG") and getattr(opened, "n_frames", 1) == 1, "A static PNG or JPEG reference is required.")
        require(type(source.get("width")) is int and type(source.get("height")) is int, "Reference dimensions must be integers.")
        require(opened.size == (source.get("width"), source.get("height")) and 0 < min(opened.size) <= max(opened.size) <= 2048,
                "Reference dimensions do not match bounded decoded pixels.")
        opened.load()
        rgba = ImageOps.exif_transpose(opened).convert("RGBA")
        flattened = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
        flattened.alpha_composite(rgba)
        contained = ImageOps.contain(flattened.convert("RGB"), (384, 256), Image.Resampling.LANCZOS)
        prepared = Image.new("RGB", (384, 256), "black")
        prepared.paste(contained, ((384 - contained.width) // 2, (256 - contained.height) // 2))
    return prepared, hashlib.sha256(prepared.tobytes()).hexdigest()


def verify_installation(job):
    root = plain(Path(job["modelDirectory"]))
    overlay = plain(Path(job["runtimeOverlayDirectory"]))
    require(root.is_absolute() and overlay == root / "_runtime" / "site-packages", "The isolated LTX overlay must belong to the selected installation.")
    owner = read_json(root / ".revia-ltx-download-owner.json")
    require(owner.get("kind") == "revia-approved-pinned-ltx-download" and owner.get("manifestSha256") == INSTALL_MANIFEST,
            "The selected model is not the approved pinned LTX installation.")
    preflight_path = root / "_runtime" / "preflight.json"
    require(digest(preflight_path) == PREFLIGHT, "The approved isolated-runtime preflight changed.")
    preflight = read_json(preflight_path)
    receipt_path = plain(Path(preflight["downloadReceipt"]))
    require(receipt_path.is_relative_to(root) and digest(receipt_path) == DOWNLOAD_RECEIPT, "The pinned download receipt changed.")
    receipt = read_json(receipt_path, 131072)
    require(receipt.get("verdict") == "verified" and receipt.get("manifestSha256") == INSTALL_MANIFEST, "The pinned installation was not verified.")
    for row in receipt["files"]:
        relative = Path(row["path"])
        require(not relative.is_absolute() and ".." not in relative.parts, "Invalid installed artifact path.")
        artifact = plain(root / relative)
        require(artifact.is_file() and artifact.stat().st_size == row["bytes"] and digest(artifact) == row["sha256"], "A pinned LTX asset changed.")
    overlay_path = root / "_runtime" / "overlay-files.json"
    require(digest(overlay_path) == OVERLAY_MANIFEST, "The frozen runtime overlay manifest changed.")
    for row in read_json(overlay_path, 262144)["files"]:
        artifact = plain(overlay / row["path"])
        require(artifact.is_relative_to(overlay) and artifact.stat().st_size == row["bytes"] and digest(artifact) == row["sha256"],
                "The installed isolated runtime changed.")


def inference_arguments(job, torch, prepared):
    arguments = {"prompt": job["prompt"], "height": 256, "width": 384, "num_frames": 49, "frame_rate": 25,
                 "num_inference_steps": 50, "guidance_scale": 3.0, "num_videos_per_prompt": 1,
                 "generator": torch.Generator(device="cuda:1" if prepared is not None else "cpu").manual_seed(job["seed"]), "output_type": "pil"}
    if prepared is not None:
        arguments["image"] = prepared
    return arguments


def generate(job):
    resources = resource_snapshot()
    require(resources["freeSystemRamBytes"] >= 45 * 1024**3, "The qualified video route requires at least45GiB free system RAM before loading.")
    minimum_free = resources["freeSystemRamBytes"]

    def measure():
        nonlocal minimum_free, resources
        resources = resource_snapshot()
        minimum_free = min(minimum_free, resources["freeSystemRamBytes"])
        require(resources["freeSystemRamBytes"] >= 8 * 1024**3, "Video generation stopped below its8GiB free system RAM guard.")
        return resources

    prepared, prepared_digest = prepare_source(job) if job["mode"] == "image_to_video" else (None, "")
    verify_installation(job)
    import torch
    import diffusers
    from transformers import T5EncoderModel, T5Tokenizer
    from diffusers import (AutoencoderKLLTXVideo, FlowMatchEulerDiscreteScheduler, LTXPipeline,
                           LTXImageToVideoPipeline, LTXVideoTransformer3DModel)

    require(diffusers.__version__ == "0.33.1" and Path(diffusers.__file__).is_relative_to(Path(job["runtimeOverlayDirectory"])),
            "The qualified isolated Diffusers runtime is not active.")
    require(torch.__version__ == "2.7.1+cu128" and torch.cuda.is_available() and torch.cuda.device_count() > 1,
            "The qualified CUDA runtime or secondary device is unavailable.")
    torch.cuda.set_device(1)
    require("RTX 2070 SUPER" in torch.cuda.get_device_name(1) and torch.cuda.get_device_capability(1) == (7, 5),
            "Device1 is not the qualified secondary GPU; no fallback is allowed.")
    torch.set_num_threads(6)
    model = Path(job["modelDirectory"])
    started = time.monotonic()
    loading = {"torch_dtype": torch.float32, "local_files_only": True, "use_safetensors": True, "low_cpu_mem_usage": True}
    components = {
        "tokenizer": T5Tokenizer.from_pretrained(str(model / "tokenizer"), local_files_only=True, trust_remote_code=False),
        "text_encoder": T5EncoderModel.from_pretrained(str(model / "text_encoder"), trust_remote_code=False, **loading),
        "transformer": LTXVideoTransformer3DModel.from_pretrained(str(model / "transformer"), **loading),
        "vae": AutoencoderKLLTXVideo.from_pretrained(str(model / "vae"), **loading),
        "scheduler": FlowMatchEulerDiscreteScheduler.from_pretrained(str(model / "scheduler"), local_files_only=True)}
    pipe = LTXPipeline(**components)
    if prepared is not None:
        converted = LTXImageToVideoPipeline.from_pipe(pipe, torch_dtype=torch.float32)
        require(all(converted.components[key] is pipe.components[key] for key in components), "The I2V conversion did not share installed components.")
        del pipe
        pipe = converted
    pipe.vae.enable_tiling(tile_sample_min_height=256, tile_sample_min_width=256, tile_sample_stride_height=192, tile_sample_stride_width=192)
    pipe.vae.use_framewise_decoding = True
    require(all(getattr(components[key], "dtype", None) == torch.float32 for key in ("text_encoder", "transformer", "vae")),
            "The actual LTX model dtype differs from qualified FP32.")
    pipe.enable_sequential_cpu_offload(gpu_id=1)
    require(str(pipe._offload_device) == "cuda:1", "The actual LTX offload device differs from the qualified secondary GPU.")
    load_ms = (time.monotonic() - started) * 1000
    actual_steps = 0
    measure()

    def step(_pipe, index, _timestep, values):
        nonlocal actual_steps
        actual_steps += 1
        print(json.dumps({"state": "generating", "step": actual_steps, **measure()}), flush=True)
        return values

    arguments = inference_arguments(job, torch, prepared)
    started = time.monotonic()
    with torch.inference_mode():
        generated = pipe(**arguments, callback_on_step_end=step)
    inference_ms = (time.monotonic() - started) * 1000
    frames = generated.frames[0]
    measure()
    require(actual_steps == 50 and len(frames) == 49 and all(frame.size == (384, 256) for frame in frames),
            "The diffusion result does not match the qualified frame and denoising plan.")
    directory = Path(job["directory"])
    frame_directory = directory / "frames"
    frame_directory.mkdir()
    for index, frame in enumerate(frames):
        frame.convert("RGB").save(frame_directory / f"frame-{index:03d}.png", format="PNG")
    pending = directory / "pending.mp4"
    started = time.monotonic()
    command = [job["ffmpegPath"], "-nostdin", "-hide_banner", "-v", "error", "-n", "-framerate", "25",
               "-i", str(frame_directory / "frame-%03d.png"), "-frames:v", "49", "-an", "-c:v", "libopenh264",
               "-b:v", "2M", "-threads", "2", "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(pending)]
    with (directory / "encode.stderr").open("xb") as error:
        completed = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=error,
            timeout=60, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    require(completed.returncode == 0 and 0 < pending.stat().st_size <= 64 * 1024 * 1024, "CPU MP4 encoding failed.")
    encoded_sha = digest(pending)
    measure()
    publish_output(pending, job["outputPath"], encoded_sha)
    source = job.get("source") or {}
    return {"path": job["outputPath"], "sha256": encoded_sha, "jobId": job["jobId"], "model": MODEL, "revision": REVISION,
            "mode": job["mode"], "seed": job["seed"], "width": 384, "height": 256, "frameCount": 49, "fps": 25,
            "durationMs": 1960, "steps": 50, "denoisingSteps": actual_steps, "provider": "local_diffusers_ltx",
            "pipeline": type(pipe).__name__, "device": "cuda:1", "dtype": "float32", "offload": "sequential_cpu", "cpuThreads": 6,
            "sourceId": source.get("id", ""), "sourceDigest": source.get("sha256", ""), "preparedSourceDigest": prepared_digest,
            "sourcePreparation": "contain-pad-rgb-black-v1" if prepared is not None else "",
            "modelLoadMilliseconds": load_ms, "inferenceMilliseconds": inference_ms,
            "encodingMilliseconds": (time.monotonic() - started) * 1000,
            "workerPid": resources["workerPid"], "peakWorkerRssMiB": resources["peakWorkerRssMiB"],
            "minimumFreeSystemRamBytes": minimum_free, "processTreeMemoryMeasured": False}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--job", required=True)
    arguments = parser.parse_args()
    job = load_job(arguments.job)
    receipt = generate(job)
    target = Path(job["directory"]) / "result.json"
    with target.open("x", encoding="utf-8") as output:
        json.dump(receipt, output, allow_nan=False)
    print(json.dumps({"state": "complete", "step": 50}), flush=True)


if __name__ == "__main__":
    main()
