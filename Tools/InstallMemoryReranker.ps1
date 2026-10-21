param(
    [string]$PythonVersion = '3.12',
    [switch]$SkipModelDownload
)

$ErrorActionPreference = 'Stop'
$rerankerRoot = Split-Path -Parent $PSScriptRoot
$runtimeDirectory = Join-Path $rerankerRoot 'ThirdParty\MemoryReranker'
$environmentDirectory = Join-Path $runtimeDirectory '.venv'
$pythonPath = Join-Path $environmentDirectory 'Scripts\python.exe'
$modelDirectory = Join-Path $rerankerRoot 'Models\MemoryReranker\ms-marco-MiniLM-L6-v2'
$launcher = Get-Command py.exe -ErrorAction Stop

New-Item -ItemType Directory -Force -Path $runtimeDirectory | Out-Null
if (-not (Test-Path -LiteralPath $pythonPath -PathType Leaf)) {
    & $launcher.Source "-$PythonVersion" -m venv $environmentDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the isolated memory reranker environment.' }
}

# CPU wheels keep this worker independent of conversation and voice GPU placement.
& $pythonPath -m pip install 'torch==2.7.1' --index-url https://download.pytorch.org/whl/cpu
if ($LASTEXITCODE -ne 0) { throw 'Could not install the pinned CPU PyTorch runtime.' }
& $pythonPath -m pip install 'sentence-transformers==5.1.2' 'transformers==4.57.1' 'safetensors==0.6.2'
if ($LASTEXITCODE -ne 0) { throw 'Could not install the pinned cross-encoder runtime.' }

if (-not $SkipModelDownload) {
    # The report's MiniLM-L-6 alias resolves to this canonical model. Download only
    # non-executable configuration/tokenizer files and safetensors from one revision.
    $downloadScript = @'
import sys
from huggingface_hub import snapshot_download
snapshot_download(repo_id="cross-encoder/ms-marco-MiniLM-L6-v2",
    revision="233902d25c440f23af6f7d6e94d2946bac0bee0a", local_dir=sys.argv[1],
    allow_patterns=["config.json", "config_sentence_transformers.json", "sentence_bert_config.json",
        "special_tokens_map.json", "tokenizer.json", "tokenizer_config.json", "vocab.txt",
        "model.safetensors"], token=False)
'@
    & $pythonPath -c $downloadScript $modelDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Could not download the reference memory reranker.' }
    $checkScript = @'
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from memory_reranker import load_model, score_request
model = load_model(Path(sys.argv[2]), 4)
result = score_request(model, {"query": "How many people live in Berlin?", "documents": [
    "Berlin had a population of 3,520,031 registered inhabitants.",
    "Berlin is well known for its museums."]})
assert result["scores"][0]["score"] > result["scores"][1]["score"], result
print("Local CPU reference reranker smoke check passed.")
'@
    & $pythonPath -c $checkScript (Join-Path $PSScriptRoot 'Workers') $modelDirectory
    if ($LASTEXITCODE -ne 0) { throw 'The installed reference reranker failed its local CPU check.' }
}

Write-Host "Memory reranker Python: $pythonPath"
Write-Host "Memory reranker model: $modelDirectory"
Write-Host 'Revia loads these files locally. It never downloads a reranker at runtime.'
