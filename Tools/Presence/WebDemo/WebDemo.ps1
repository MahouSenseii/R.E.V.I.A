param(
    [ValidateSet('start', 'revia', 'bridge', 'check', 'status-local', 'status-relay', 'help')]
    [string]$Action = 'help',
    [string]$ConfigDirectory = $PSScriptRoot,
    [string]$Executable
)
$ErrorActionPreference = 'Stop'
$node = (Get-Command node -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$arguments = @((Join-Path $PSScriptRoot 'operator\main.js'), $Action, '--config-dir', $ConfigDirectory)
if ($Executable) { $arguments += @('--exe', $Executable) }
& $node @arguments
if ($LASTEXITCODE -ne 0) { throw 'Web demo command failed. See the operator guide.' }
