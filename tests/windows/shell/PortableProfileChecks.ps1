# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Production profile selection, environment, panel names and updater handoff.
# Real filesystem/mutexes; only PE metadata and installer process creation are
# fixtures. No Android runtime, network request, registry write or WPF required.
param([string] $PackageDirectory = (Join-Path $PSScriptRoot '../../../tools/package'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory)
. (Join-Path $PackageDirectory 'gesher-runtime-files.ps1')
. (Join-Path $PackageDirectory 'gesher-update.ps1')
$script:checks = 0
function Assert-Profile([bool] $condition, [string] $message) {
    if (-not $condition) { throw $message }
    $script:checks++
}
$saved = @{}
foreach ($name in 'GESHER_PROFILE', 'GESHER_PORTABLE', 'LOCALAPPDATA', 'ANDROID_DATA') {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$work = Join-Path ([IO.Path]::GetTempPath()) ('gesher-profile-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $env:LOCALAPPDATA = Join-Path $work 'host-profile'
    $default = Join-Path $env:LOCALAPPDATA 'Gesher'
    $normal = Join-Path $work 'regular'
    $flat = Join-Path $work 'portable-flat'
    $zip = Join-Path $work 'portable-zip'
    $app = Join-Path $zip 'App'
    foreach ($directory in $normal, $flat, $app) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
    [IO.File]::WriteAllText((Join-Path $flat 'portable.marker'), '')
    [IO.File]::WriteAllText((Join-Path $zip 'portable.marker'), '')
    $env:GESHER_PROFILE = $null
    Assert-Profile ((Get-GesherProfileRoot $flat) -ceq (Join-Path $flat 'Data')) 'Flat portable first start did not select Data'
    Assert-Profile ((Get-GesherProfileRoot $app) -ceq (Join-Path $zip 'Data')) 'ZIP App first start did not select root Data'
    Assert-Profile (-not (Test-Path -LiteralPath (Join-Path $zip 'Data'))) 'Resolution created profile state'
    Assert-Profile ((Get-GesherProfileRoot $normal) -ceq $default) 'Regular install lost its default profile'
    Assert-Profile ((Get-GesherProfileRoot '') -ceq $default) 'Missing install directory changed the normal default'

    # A directory named Data is not a portable declaration, anywhere in the tree.
    foreach ($directory in (Join-Path $work 'Data'), (Join-Path $normal 'Data')) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    Assert-Profile ((Get-GesherProfileRoot $normal) -ceq $default) 'Unrelated Data captured a regular installation'
    $other = Join-Path $zip 'Other'
    New-Item -ItemType Directory -Path $other | Out-Null
    Assert-Profile ((Get-GesherProfileRoot $other) -ceq $default) 'A parent marker captured an unrelated nested installation'
    New-Item -ItemType Directory -Path (Join-Path $normal 'portable.marker') | Out-Null
    Assert-Profile ((Get-GesherProfileRoot $normal) -ceq $default) 'A directory named portable.marker enabled portable mode'
    [IO.File]::WriteAllText((Join-Path $app 'portable.marker'), '')
    Assert-Profile ((Get-GesherProfileRoot $app) -ceq (Join-Path $app 'Data')) 'Own marker did not take precedence over App parent marker'
    Remove-Item -LiteralPath (Join-Path $app 'portable.marker')

    $explicit = Join-Path $work 'not-created-yet'
    $env:GESHER_PROFILE = $explicit
    $env:ANDROID_DATA = Join-Path $work 'stale-data'
    Assert-Profile ((Get-GesherProfileRoot $app) -ceq $explicit) 'A new explicit profile was ignored'
    Assert-Profile ((Set-GesherProfileEnvironment $app) -ceq $explicit) 'Initialization discarded the explicit profile'
    Assert-Profile ($env:GESHER_PORTABLE -ceq '1') 'Explicit profile disabled portable integration suppression'
    Assert-Profile (-not (Test-Path -LiteralPath $explicit)) 'Environment selection created an explicit profile prematurely'
    $env:GESHER_PROFILE = $null
    $null = Set-GesherProfileEnvironment $normal
    Assert-Profile ($env:GESHER_PROFILE -ceq $default) 'Regular initialization picked stale Android data'
    Assert-Profile ($env:GESHER_PORTABLE -ceq '0') 'Regular initialization inherited portable mode'

    # Execute the actual initialization assignment from all three entrypoints.
    foreach ($file in 'gesher-app.ps1', 'gesher.ps1', 'gesher-control-panel.ps1') {
        $errors = $null
        $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PackageDirectory $file), [ref]$null, [ref]$errors)
        Assert-Profile (-not $errors.Count) "Parse failed: $file"
        $assignment = @($ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left.Extent.Text -ceq '$root' -and $node.Right.Extent.Text -match 'Set-GesherProfileEnvironment'
        }, $true))
        Assert-Profile ($assignment.Count -eq 1) "Entrypoint does not initialize mode and profile together: $file"
        $here = $app; $env:GESHER_PROFILE = $null; $env:GESHER_PORTABLE = '0'
        . ([scriptblock]::Create($assignment[0].Extent.Text))
        Assert-Profile ($root -ceq (Join-Path $zip 'Data') -and $env:GESHER_PORTABLE -ceq '1') "Entrypoint portable environment differs: $file"
        $here = $normal; $env:GESHER_PROFILE = $explicit
        . ([scriptblock]::Create($assignment[0].Extent.Text))
        Assert-Profile ($root -ceq $explicit -and $env:GESHER_PORTABLE -ceq '0') "Entrypoint regular override differs: $file"
    }

    $normalName = Get-GesherControlPanelName $default
    $portableName = Get-GesherControlPanelName (Join-Path $zip 'Data')
    Assert-Profile ($normalName -ceq 'Local\Gesher.ControlPanel') 'Default panel name changed across upgrades'
    Assert-Profile ($portableName -cne $normalName) 'Portable and installed panels share a mutex'
    Assert-Profile ($portableName -cne (Get-GesherControlPanelName (Join-Path $flat 'Data'))) 'Two USB profiles share a panel mutex'
    Assert-Profile ($portableName -ceq (Get-GesherControlPanelName ((Join-Path $zip 'Data') + [IO.Path]::DirectorySeparatorChar))) 'Trailing separator changed panel identity'
    Assert-Profile ($portableName -ceq (Get-GesherControlPanelName ((Join-Path $zip 'Data').ToUpperInvariant()))) 'Windows path case changed panel identity'
    $first = $null; $duplicate = $null; $different = $null
    try {
        $created = $false
        $first = [Threading.Mutex]::new($false, $portableName, [ref]$created)
        Assert-Profile $created 'First profile mutex was unexpectedly held'
        $duplicate = [Threading.Mutex]::new($false, $portableName, [ref]$created)
        Assert-Profile (-not $created) 'Same-profile panel did not find its existing mutex'
        $different = [Threading.Mutex]::new($false, (Get-GesherControlPanelName $explicit), [ref]$created)
        Assert-Profile $created 'Different-profile panel was captured by existing mutex'
    } finally {
        foreach ($mutex in $first, $duplicate, $different) { if ($mutex) { $mutex.Dispose() } }
    }

    # Exercise the real final size/hash check and command construction. Only the
    # PE version reader and process boundary are substituted; bytes stay real.
    function Get-GesherUpdateInstallerVersion([string] $Path) { return '0.2.0' }
    function Start-GesherUpdateProcess([string] $Path, [string] $Arguments, [string] $WorkingDirectory) {
        $script:started = $Arguments
        $script:starts++
        return [pscustomobject]@{ Path = $Path; WorkingDirectory = $WorkingDirectory }
    }
    $downloadDirectory = Join-Path $work 'downloads'
    New-Item -ItemType Directory -Path $downloadDirectory | Out-Null
    $payload = Join-Path $downloadDirectory 'gesher-setup-0.2.0.exe'
    [IO.File]::WriteAllText($payload, 'verified fixture bytes; never executed')
    $release = [pscustomobject]@{
        Version = '0.2.0'; Tag = 'v0.2.0'; AssetName = 'gesher-setup-0.2.0.exe'
        Url = 'https://github.com/gesher-runtime/gesher/releases/tag/v0.2.0'
        DownloadUrl = 'https://github.com/gesher-runtime/gesher/releases/download/v0.2.0/gesher-setup-0.2.0.exe'
        Size = (Get-Item -LiteralPath $payload).Length
        Sha256 = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash
    }
    $download = [pscustomobject]@{ Path = $payload; Directory = $downloadDirectory; Release = $release }
    $script:starts = 0
    foreach ($directory in $normal, $flat, $app) {
        [IO.File]::WriteAllText((Join-Path $directory 'gesher-version.ini'), "[Gesher]`nVersion=0.1.0`n")
        $null = Start-GesherUpdateInstaller -Download $download -InstallDirectory $directory
        $mode = if ($directory -eq $normal) { '' } else { ' /PORTABLE' }
        Assert-Profile ($script:started -ceq ('/NORESTART' + $mode + ' /DIR="' + $directory + '"')) "Updater changed mode or moved the runtime directory: $directory"
    }
    Assert-Profile ($script:starts -eq 3) 'Updater did not perform exactly the requested handoffs'
    [IO.File]::WriteAllText($payload, 'corrupt')
    $failed = $false
    try { $null = Start-GesherUpdateInstaller -Download $download -InstallDirectory $app } catch { $failed = $true }
    Assert-Profile ($failed -and $script:starts -eq 3) 'Portable update bypassed payload integrity'
    Assert-Profile (-not (Test-Path -LiteralPath $env:LOCALAPPDATA)) 'Portable profile tests wrote to the host profile'
    Write-Host "Portable profile checks passed ($script:checks)."
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    Remove-Item -LiteralPath $work -Recurse -Force
}
