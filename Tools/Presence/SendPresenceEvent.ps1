param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("discord", "stream", "game")]
    [string]$Source,

    [Parameter(Mandatory = $true)]
    [ValidateLength(1, 80)]
    [string]$Channel,

    [Parameter(Mandatory = $true)]
    [ValidateLength(1, 120)]
    [string]$Author,

    [Parameter(Mandatory = $true)]
    [ValidateLength(1, 4000)]
    [string]$Text
)

$runtimeRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$inbox = Join-Path $runtimeRoot "RuntimeData\Presence\Inbox"
New-Item -ItemType Directory -Force -Path $inbox | Out-Null

$eventId = [guid]::NewGuid().ToString("N")
$target = Join-Path $inbox ("{0}-{1}.json" -f $Source, $eventId)
$temporary = "$target.tmp"
$payload = [ordered]@{
    version = 1
    id = $eventId
    source = $Source
    channel = $Channel
    author = $Author
    text = $Text
}
$payload | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $temporary -Encoding UTF8
Move-Item -LiteralPath $temporary -Destination $target
Write-Output "Queued presence event $eventId at $target"
