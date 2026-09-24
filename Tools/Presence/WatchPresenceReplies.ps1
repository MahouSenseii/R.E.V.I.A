param(
    [ValidateSet("discord", "stream", "game")]
    [string]$Source = "discord",

    [int]$PollMilliseconds = 200
)

$runtimeRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outbox = Join-Path $runtimeRoot "RuntimeData\Presence\Outbox"
New-Item -ItemType Directory -Force -Path $outbox | Out-Null
$seen = @{}
Write-Output "Watching $outbox for $Source replies. Press Ctrl+C to stop."

while ($true) {
    Get-ChildItem -LiteralPath $outbox -Filter "$Source-reply-*.json" -File |
        Sort-Object LastWriteTimeUtc |
        ForEach-Object {
            if (-not $seen.ContainsKey($_.FullName)) {
                $seen[$_.FullName] = $true
                Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json
            }
        }
    Start-Sleep -Milliseconds ([Math]::Max(50, $PollMilliseconds))
}
