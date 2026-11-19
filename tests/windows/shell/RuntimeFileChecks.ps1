param([string] $HelperPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-runtime-files.ps1'))

$ErrorActionPreference = 'Stop'
. $HelperPath
$script:Checks = 0
$script:CopyCalls = 0
# Observe real copy attempts as well as Windows' read-sharing behavior. This
# makes the no-rewrite contract visible on hosts that do not enforce NT sharing.
function Copy-Item {
    [CmdletBinding()]
    param([string] $LiteralPath, [string] $Destination)
    $script:CopyCalls++
    Microsoft.PowerShell.Management\Copy-Item @PSBoundParameters
}
function Assert-RuntimeFile([bool] $Value, [string] $Message) {
    if (-not $Value) { throw $Message }
    $script:Checks++
}
$work = Join-Path ([IO.Path]::GetTempPath()) ('gesher-runtime-files-' + [guid]::NewGuid())
try {
    $install = Join-Path $work 'install [fixture]'
    $profile = Join-Path $work 'profile'
    $null = [IO.Directory]::CreateDirectory($install)
    $null = [IO.Directory]::CreateDirectory((Join-Path $profile 'system'))
    $null = [IO.Directory]::CreateDirectory((Join-Path $profile 'data/app/fixture'))
    $source = Join-Path $install 'build.prop'
    $destination = Join-Path $profile 'system/build.prop'
    $userData = Join-Path $profile 'data/app/fixture/user.txt'
    [IO.File]::WriteAllText($userData, 'keep user data')
    $metadata = Join-Path $install 'gesher-version.ini'
    [IO.File]::WriteAllText($metadata, "[Gesher]`r`nVersion=0.1.9`r`n")
    [IO.File]::WriteAllText($source, 'old data')
    $state = New-GesherRuntimeSync $install $profile app
    Assert-RuntimeFile $state.ForceRefresh 'First versioned launch must initialize runtime files'
    Sync-GesherRuntimeFile $source $destination $state
    Complete-GesherRuntimeSync $state
    Assert-RuntimeFile ([IO.File]::ReadAllText($destination) -ceq 'old data') 'Initial copy did not populate runtime file'

    $stampTime = [IO.File]::GetLastWriteTimeUtc($state.StampPath)
    $stable = New-GesherRuntimeSync $install $profile app
    Assert-RuntimeFile (-not $stable.ForceRefresh) 'Same version invalidated the runtime cache'
    # A read-only handle on Windows proves unchanged launches do not rewrite a mapped file.
    $handle = [IO.File]::Open($destination, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try { Sync-GesherRuntimeFile $source $destination $stable } finally { $handle.Dispose() }
    Complete-GesherRuntimeSync $stable
    Assert-RuntimeFile ([IO.File]::GetLastWriteTimeUtc($state.StampPath) -eq $stampTime) 'Ordinary launch rewrote generation stamp'

    $timestamp = [IO.File]::GetLastWriteTimeUtc($source)
    [IO.File]::WriteAllText($source, 'new data')
    [IO.File]::SetLastWriteTimeUtc($source, $timestamp)
    [IO.File]::WriteAllText($metadata, "[Gesher]`nVersion=0.1.10`n")
    $updated = New-GesherRuntimeSync $install $profile app
    Assert-RuntimeFile $updated.ForceRefresh 'New version did not invalidate runtime files'
    Sync-GesherRuntimeFile $source $destination $updated
    Complete-GesherRuntimeSync $updated
    Assert-RuntimeFile ([IO.File]::ReadAllText($destination) -ceq 'new data') 'Equal-size, equal-time resource stayed stale after upgrade'
    Assert-RuntimeFile ([IO.File]::ReadAllText($userData) -ceq 'keep user data') 'Runtime upgrade changed app data'

    $dex = New-GesherRuntimeSync $install $profile dex
    Assert-RuntimeFile $dex.ForceRefresh 'App completion incorrectly acknowledged the dex component'
    $copies = $script:CopyCalls
    $handle = [IO.File]::Open($destination, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        Sync-GesherRuntimeFile $source $destination $dex
        Complete-GesherRuntimeSync $dex
    } finally { $handle.Dispose() }
    Assert-RuntimeFile ($script:CopyCalls -eq $copies) 'Other launcher rewrote an identical, possibly mapped runtime file'
    Assert-RuntimeFile (-not (New-GesherRuntimeSync $install $profile dex).ForceRefresh) 'Other launcher could not acknowledge an identical, read-shared runtime file'
    # A release may preserve ICU bytes but rebuild with different timestamps.
    # This is still not a reason to overwrite a file an existing app has mapped.
    [IO.File]::SetLastWriteTimeUtc($source, $timestamp.AddMinutes(1))
    $handle = [IO.File]::Open($destination, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try { Sync-GesherRuntimeFile $source $destination $dex } finally { $handle.Dispose() }
    Assert-RuntimeFile ($script:CopyCalls -eq $copies) 'Timestamp-only difference rewrote identical, possibly mapped runtime bytes'
    Assert-RuntimeFile ([IO.File]::ReadAllText($destination) -ceq 'new data') 'Identical resource bytes changed during timestamp reconciliation'
    [IO.File]::WriteAllText($metadata, "[Gesher]`nVersion=0.1.11`n")
    $dex = New-GesherRuntimeSync $install $profile dex
    Complete-GesherRuntimeSync $dex
    $app = New-GesherRuntimeSync $install $profile app
    Assert-RuntimeFile $app.ForceRefresh 'Dex completion incorrectly acknowledged the full framework upgrade'

    $failed = $false
    try { Sync-GesherRuntimeFile (Join-Path $install 'missing') $destination $app } catch { $failed = $true }
    Assert-RuntimeFile $failed 'Missing runtime source was treated as a successful copy'
    $retry = New-GesherRuntimeSync $install $profile app
    Assert-RuntimeFile $retry.ForceRefresh 'Failed synchronization prevented retry'

    # Corrupt version state recovers by re-copying; unversioned development builds
    # compare real content even when both length and modification time match.
    [IO.File]::WriteAllText($app.StampPath, 'corrupt')
    Assert-RuntimeFile (New-GesherRuntimeSync $install $profile app).ForceRefresh 'Corrupt cache stamp was trusted'
    [IO.File]::Delete($metadata)
    [IO.File]::WriteAllText($source, 'dev data')
    [IO.File]::SetLastWriteTimeUtc($source, [IO.File]::GetLastWriteTimeUtc($destination))
    $development = New-GesherRuntimeSync $install $profile app
    Sync-GesherRuntimeFile $source $destination $development
    Assert-RuntimeFile ([IO.File]::ReadAllText($destination) -ceq 'dev data') 'Unversioned same-size resource did not refresh'
    Assert-RuntimeFile ([IO.File]::ReadAllText($userData) -ceq 'keep user data') 'Developer synchronization changed app data'
    foreach ($invalid in @(
        "Version=0.1.10`n",
        "[Other]`nVersion=0.1.10`n",
        "[Gesher]`nVersion=0.1.10`nVersion=0.1.11`n",
        "[Gesher]`nVersion=0.1.10`n[Gesher]`nVersion=0.1.10`n",
        "[Gesher]`nVersion=0.1.65536`n",
        "[Gesher]`nVersion=0.1.10.0`n",
        "[Gesher]`nVersion=0.1.10.65536`n",
        "[Gesher]`nVersion=0.1.10.1.2`n",
        "[Gesher]`nVersion=00.1.10`n"
    )) {
        [IO.File]::WriteAllText($metadata, $invalid)
        Assert-RuntimeFile ($null -eq (New-GesherRuntimeSync $install $profile app).Generation) 'Invalid installed version metadata was accepted as a cache identity'
    }
    # A CI build (X.Y.Z.N) is its own installation generation, distinct from its release.
    [IO.File]::WriteAllText($metadata, "[Gesher]`nVersion=0.1.10`n")
    $releaseGeneration = (New-GesherRuntimeSync $install $profile app).Generation
    [IO.File]::WriteAllText($metadata, "[Gesher]`nVersion=0.1.10.526`n")
    $buildGeneration = (New-GesherRuntimeSync $install $profile app).Generation
    Assert-RuntimeFile ($buildGeneration -and $buildGeneration -cne $releaseGeneration -and $buildGeneration.EndsWith("`n0.1.10.526")) 'A CI build version was not a distinct cache identity'

    # The installed launcher can run in a Windows PowerShell environment where
    # Get-FileHash is unavailable. Exercise the production helper in a fresh
    # runspace with only filesystem commands and no automatic module loading.
    $initialState = [Management.Automation.Runspaces.InitialSessionState]::CreateDefault2()
    $initialState.ImportPSModule(@('Microsoft.PowerShell.Management'))
    $runspace = [RunspaceFactory]::CreateRunspace($initialState)
    $worker = $null
    try {
        $runspace.Open()
        $worker = [PowerShell]::Create()
        $worker.Runspace = $runspace
        $null = $worker.AddScript({
            param($Helper, $Root)
            $ErrorActionPreference = 'Stop'
            $PSModuleAutoLoadingPreference = 'None'
            $hashCommand = $ExecutionContext.SessionState.InvokeCommand.GetCommand(
                'Get-FileHash', [Management.Automation.CommandTypes]::All)
            if ($null -ne $hashCommand) { throw 'Regression runspace unexpectedly provides Get-FileHash' }
            . $Helper
            $install = Join-Path $Root 'install [isolated]'
            $profile = Join-Path $Root 'profile'
            $null = [IO.Directory]::CreateDirectory($install)
            $null = [IO.Directory]::CreateDirectory((Join-Path $profile 'system'))
            $source = Join-Path $install 'build.prop'
            $destination = Join-Path $profile 'system/build.prop'
            $metadata = Join-Path $install 'gesher-version.ini'
            [IO.File]::WriteAllText($metadata, "[Gesher]`nVersion=0.2.0`n")
            [IO.File]::WriteAllText($source, 'old data')
            [IO.File]::WriteAllText($destination, 'old data')
            # PowerShell's provider location differs from the process directory.
            # Relative literal paths must still resolve through Get-Item first.
            # Windows PowerShell 5.1 escapes brackets in the current location
            # even for LiteralPath (PowerShell/PowerShell#5896). Keep brackets
            # in the relative source while using its plain parent as location.
            Set-Location -LiteralPath $Root
            $relativeSource = 'install [isolated]/build.prop'
            $relativeDestination = 'profile/system/build.prop'
            $differentDirectories = [IO.Directory]::GetCurrentDirectory() -ine $Root
            $state = New-GesherRuntimeSync $install $profile app
            $mapped = [IO.File]::Open($destination, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            try {
                Sync-GesherRuntimeFile $relativeSource $relativeDestination $state
                Complete-GesherRuntimeSync $state
            } finally { $mapped.Dispose() }
            $identical = [IO.File]::ReadAllText($destination) -ceq 'old data'
            $previousStamp = [IO.File]::ReadAllText($state.StampPath)

            [IO.File]::WriteAllText($source, 'new data')
            [IO.File]::SetLastWriteTimeUtc($source, [IO.File]::GetLastWriteTimeUtc($destination))
            [IO.File]::WriteAllText($metadata, "[Gesher]`nVersion=0.2.1`n")
            $state = New-GesherRuntimeSync $install $profile app
            $readFailed = $false
            $exclusive = [IO.File]::Open($destination, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
            try {
                try {
                    Sync-GesherRuntimeFile $relativeSource $relativeDestination $state
                    Complete-GesherRuntimeSync $state
                } catch { $readFailed = $true }
            } finally { $exclusive.Dispose() }
            $stampPreserved = [IO.File]::ReadAllText($state.StampPath) -ceq $previousStamp
            $retry = New-GesherRuntimeSync $install $profile app
            # A failed destination read must also release the source hash handle.
            $exclusive = [IO.File]::Open($source, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            $exclusive.Dispose()
            Sync-GesherRuntimeFile $relativeSource $relativeDestination $retry
            Complete-GesherRuntimeSync $retry
            $updated = [IO.File]::ReadAllText($destination) -ceq 'new data'
            # Both success and failure paths must release their streams.
            foreach ($path in @($source, $destination)) {
                $exclusive = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
                $exclusive.Dispose()
            }
            [PSCustomObject]@{
                HashUnavailable = ($null -eq $hashCommand)
                RelativePaths = $differentDirectories
                Identical = $identical
                ReadFailed = $readFailed
                StampPreserved = $stampPreserved
                Retried = ($retry.ForceRefresh -and $updated)
                Completed = (-not (New-GesherRuntimeSync $install $profile app).ForceRefresh)
                StreamsReleased = $true
            }
        }).AddArgument([IO.Path]::GetFullPath($HelperPath)).AddArgument((Join-Path $work 'isolated'))
        $result = @($worker.Invoke())
        if ($worker.Streams.Error.Count) { throw ($worker.Streams.Error | Out-String) }
        Assert-RuntimeFile ($result.Count -eq 1 -and $result[0].HashUnavailable) 'Clean runspace did not exclude Get-FileHash'
        Assert-RuntimeFile $result[0].RelativePaths 'Relative-path regression did not separate provider and process directories'
        Assert-RuntimeFile $result[0].Identical 'Module-independent synchronization changed identical mapped bytes'
        Assert-RuntimeFile $result[0].ReadFailed 'Hash read failure was treated as successful synchronization'
        Assert-RuntimeFile $result[0].StampPreserved 'Hash read failure changed the completed generation marker'
        Assert-RuntimeFile $result[0].Retried 'Module-independent retry left an equal-size, equal-time resource stale'
        Assert-RuntimeFile $result[0].Completed 'Module-independent synchronization did not complete its generation'
        Assert-RuntimeFile $result[0].StreamsReleased 'Module-independent hashing retained a file handle'
    } finally {
        if ($worker) { $worker.Dispose() }
        $runspace.Dispose()
    }
    'PASS: runtime file upgrade contracts ({0} checks)' -f $script:Checks
} finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
