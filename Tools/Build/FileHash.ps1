function Get-ReviaFileHash
{
    param([Parameter(Mandatory = $true, ValueFromPipeline = $true)][object]$LiteralPath)
    process
    {
        $path = if ($LiteralPath -is [IO.FileInfo]) { $LiteralPath.FullName } else { (Resolve-Path -LiteralPath ([string]$LiteralPath)).ProviderPath }
        $stream = [IO.File]::OpenRead($path)
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try
        {
            $hash = [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '')
            [pscustomobject]@{ Algorithm = 'SHA256'; Path = $path; Hash = $hash }
        }
        finally
        {
            $algorithm.Dispose()
            $stream.Dispose()
        }
    }
}
