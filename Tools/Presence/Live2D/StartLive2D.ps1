param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$StatePath,
    [switch]$Inspect,
    [string]$Endpoint,
    [string]$AuthFile
)

$ErrorActionPreference = 'Stop'

try
{
    $nodeExecutable = Get-Command -Name 'node' -CommandType Application -ErrorAction Stop | Select-Object -First 1
}
catch
{
    Write-Error 'Node.js 22.12 or newer must be available on PATH.'
    exit 1
}

$nodeArguments = @((Join-Path $PSScriptRoot 'main.mjs'), '--state', $StatePath)
if ($Endpoint)
{
    $nodeArguments += @('--endpoint', $Endpoint)
}
if ($AuthFile)
{
    $nodeArguments += @('--auth-file', $AuthFile)
}
if ($Inspect)
{
    $nodeArguments += '--inspect'
}

& $nodeExecutable.Source @nodeArguments
exit $LASTEXITCODE
