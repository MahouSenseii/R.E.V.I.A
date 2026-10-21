param([string]$PythonVersion = '3.12')

$ErrorActionPreference = 'Stop'
$reviaNliRoot = Split-Path -Parent $PSScriptRoot
$reviaNliPython = Join-Path $reviaNliRoot 'ThirdParty\MemoryReranker\.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $reviaNliPython -PathType Leaf)) {
    & (Join-Path $PSScriptRoot 'InstallMemoryReranker.ps1') -PythonVersion $PythonVersion -SkipModelDownload
}
if (-not (Test-Path -LiteralPath $reviaNliPython -PathType Leaf)) {
    throw 'The pinned local CPU cross-encoder environment is unavailable.'
}
$reviaNliModel = Join-Path $reviaNliRoot 'Models\MemoryNli\nli-deberta-v3-small'
$reviaNliDownload = @'
import hashlib
import json
from pathlib import Path
import sys
from huggingface_hub import snapshot_download
directory = Path(sys.argv[1])
revision = "fa2804872c3b4bd748f38c0185cc85775361e735"
allowed = ["config.json", "tokenizer.json", "tokenizer_config.json",
           "special_tokens_map.json", "added_tokens.json", "spm.model", "model.safetensors"]
snapshot_download(repo_id="cross-encoder/nli-deberta-v3-small", revision=revision,
                  local_dir=directory, allow_patterns=allowed, token=False)
config = json.loads((directory / "config.json").read_text(encoding="utf-8"))
assert config["id2label"] == {"0": "contradiction", "1": "entailment", "2": "neutral"}
records = []
for name in allowed:
    path = directory / name
    if path.is_file():
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for block in iter(lambda: source.read(4 * 1024 * 1024), b""):
                digest.update(block)
        records.append({"file": name, "bytes": path.stat().st_size, "sha256": digest.hexdigest()})
assert any(row["file"] == "model.safetensors" for row in records)
assert next(row["sha256"] for row in records if row["file"] == "model.safetensors") == \
    "ebc79588dd73ccfb6a3f6078519cfbf512c5305384c5ea1845bc71cd32216e86"
(directory / "installation.json").write_text(json.dumps({
    "model": "cross-encoder/nli-deberta-v3-small", "revision": revision, "files": records
}, indent=2) + "\n", encoding="utf-8")
print("Pinned NLI safetensors and tokenizer files downloaded and hashed.")
'@
& $reviaNliPython -c $reviaNliDownload $reviaNliModel
if ($LASTEXITCODE -ne 0) { throw 'Could not install the pinned local NLI artifact.' }
Write-Host 'Installation does not enable duplicate marking. Run the labelled evaluation before considering activation.'
