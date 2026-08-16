param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$modelsDirectory = Join-Path $repoRoot 'Models'
$destination = Join-Path $modelsDirectory 'nomic-embed-text-v1.5.Q4_K_M.gguf'
$downloadPath = "$destination.download"
$expectedHash = 'd4e388894e09cf3816e8b0896d81d265b55e7a9fff9ab03fe8bf4ef5e11295ac'
$modelUrl = 'https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/main/nomic-embed-text-v1.5.Q4_K_M.gguf?download=true'

New-Item -ItemType Directory -Path $modelsDirectory -Force | Out-Null

if (Test-Path -LiteralPath $destination -PathType Leaf) {
    $existingHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($existingHash -eq $expectedHash) {
        Write-Host "Embedding model is already verified: $destination"
        exit 0
    }
    throw "The existing embedding model has an unexpected SHA-256. Move it aside before retrying: $destination"
}

Invoke-WebRequest -Uri $modelUrl -OutFile $downloadPath
$downloadHash = (Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($downloadHash -ne $expectedHash) {
    throw "Embedding model SHA-256 verification failed. The untrusted download remains at: $downloadPath"
}

Move-Item -LiteralPath $downloadPath -Destination $destination
Write-Host "Verified embedding model ready: $destination"
