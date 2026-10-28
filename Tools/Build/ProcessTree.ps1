function Get-ReviaProcessTree([object[]]$Snapshot, [int]$RootId, [DateTime]$RootCreatedUtc)
{
    $root = $Snapshot | Where-Object { $_.ProcessId -eq $RootId } | Select-Object -First 1
    if (-not $root -or -not $root.CreationDate) { return @() }
    $created = ([DateTime]$root.CreationDate).ToUniversalTime()
    # CIM birth times have microsecond precision; .NET process times use 100 ns ticks.
    if ([Math]::Abs(($created - $RootCreatedUtc.ToUniversalTime()).TotalMilliseconds) -gt 1) { return @() }
    $descendants = @{ $RootId = $root }
    do
    {
        $previousCount = $descendants.Count
        foreach ($entry in $Snapshot)
        {
            $id = [int]$entry.ProcessId
            $parentId = [int]$entry.ParentProcessId
            if ($descendants.ContainsKey($id) -or -not $descendants.ContainsKey($parentId) -or -not $entry.CreationDate) { continue }
            $parentCreated = ([DateTime]$descendants[$parentId].CreationDate).ToUniversalTime()
            if (([DateTime]$entry.CreationDate).ToUniversalTime() -ge $parentCreated) { $descendants[$id] = $entry }
        }
    } while ($descendants.Count -gt $previousCount)
    return @($descendants.Values)
}
