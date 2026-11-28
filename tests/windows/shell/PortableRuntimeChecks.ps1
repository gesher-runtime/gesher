# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Actual packaged Windows/ART smoke. Run with pwsh after the ordinary installer
# has been installed and its version verified. No registry/shortcut fixtures are
# written or removed. Notifications may still register AppUserModelIds; this
# checks portable app shortcuts/protocols and preservation of the normal install,
# not an absence of every Windows trace.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $ZipPath,
    [Parameter(Mandatory)][string] $InstallerPath,
    [Parameter(Mandatory)][string] $Version,
    [string] $InstalledRuntimeDirectory = (Join-Path $env:LOCALAPPDATA 'Programs\Gesher'),
    # The package job's verified dist/components.apk has a BROWSABLE custom
    # scheme. lifecycle.apk alone cannot exercise protocol registration.
    [string] $ComponentsApk = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or $PSVersionTable.PSVersion.Major -lt 7) {
    throw 'PortableRuntimeChecks requires Windows and pwsh 7; it is not a host-only fixture.'
}
$ZipPath = (Resolve-Path -LiteralPath $ZipPath).ProviderPath
$InstallerPath = (Resolve-Path -LiteralPath $InstallerPath).ProviderPath
$InstalledRuntimeDirectory = (Resolve-Path -LiteralPath $InstalledRuntimeDirectory).ProviderPath
if ($ComponentsApk) { $ComponentsApk = (Resolve-Path -LiteralPath $ComponentsApk).ProviderPath }
$testStarted = [DateTime]::UtcNow
$tempRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$work = Join-Path ([IO.Path]::GetFullPath($tempRoot)) ('gesher-portable-runtime-' + [guid]::NewGuid().ToString('N'))
$logs = Join-Path $work 'logs'
$null = New-Item -ItemType Directory -Path $logs
$script:checks = 0
$script:sequence = 0
$ownedRoots = [Collections.Generic.List[string]]::new()
$failure = $null
$receipt = $null
$cleanupFailures = [Collections.Generic.List[string]]::new()
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$uninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6A1F3C2E-5B7D-4E8A-9C0F-1D2E3F4A5B6C}_is1'

function Assert-PortableRuntime([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function File-Hash([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}
function Is-InDirectory([string] $Path, [string] $Directory) {
    $pathFull = [IO.Path]::GetFullPath($Path)
    $directoryFull = [IO.Path]::GetFullPath($Directory)
    # Runtime paths can use Windows' 8.3 spelling. Expand existing paths before
    # comparing, including process images discovered during bounded cleanup.
    if ('Gesher.PortableRuntimePaths' -as [type]) {
        $buffer = [Text.StringBuilder]::new(32768)
        $length = [Gesher.PortableRuntimePaths]::GetLongPathName($pathFull, $buffer, $buffer.Capacity)
        if ($length -gt 0 -and $length -lt $buffer.Capacity) { $pathFull = $buffer.ToString() }
        $null = $buffer.Clear()
        $length = [Gesher.PortableRuntimePaths]::GetLongPathName($directoryFull, $buffer, $buffer.Capacity)
        if ($length -gt 0 -and $length -lt $buffer.Capacity) { $directoryFull = $buffer.ToString() }
    }
    $prefix = $directoryFull.TrimEnd('\') + '\'
    return $pathFull.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}
function Registry-Tree([string] $Path) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($Path, $false)
    if ($null -eq $key) { return [ordered]@{ Path = $Path; Exists = $false } }
    try {
        $values = @()
        foreach ($name in @($key.GetValueNames() | Sort-Object)) {
            $values += [ordered]@{ Name = $name; Kind = [string]$key.GetValueKind($name)
                Value = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
        }
        $children = @()
        foreach ($name in @($key.GetSubKeyNames() | Sort-Object)) { $children += Registry-Tree ($Path + '\' + $name) }
        return [ordered]@{ Path = $Path; Exists = $true; Values = $values; Children = $children }
    } finally { $key.Dispose() }
}
function Host-State {
    $keys = @($uninstallKey, 'Software\Classes\.apk', 'Software\Classes\.xapk',
        'Software\Classes\.apks', 'Software\Classes\.apkm', 'Software\Classes\.apk+',
        'Software\Classes\gesher-components',
        'Software\Classes\CLSID\{6B0C3C9E-2D1B-4E57-9B7A-3F2C8E4D1A50}',
        'Software\Classes\CLSID\{C18D9445-A7F0-472E-985F-AC354EBEB660}')
    $classes = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Classes', $false)
    if ($classes) {
        try { $keys += @($classes.GetSubKeyNames() | Where-Object { $_ -like 'Gesher.*' } | ForEach-Object { 'Software\Classes\' + $_ }) }
        finally { $classes.Dispose() }
    }
    $registry = @($keys | Sort-Object -Unique | ForEach-Object { Registry-Tree $_ })
    $programs = [Environment]::GetFolderPath('Programs')
    $shortcuts = @()
    # Preserve the installed panel/readme links and any existing Android-app
    # links. A portable install must neither create nor overwrite/delete them.
    foreach ($item in @(Get-ChildItem -LiteralPath $programs -Force | Where-Object { $_.Name -match 'Gesher|גשר' } | Sort-Object FullName)) {
        $shortcuts += [ordered]@{ Path = $item.FullName; Directory = $item.PSIsContainer; Hash = $(if (-not $item.PSIsContainer) { File-Hash $item.FullName }) }
        if ($item.PSIsContainer) {
            foreach ($file in @(Get-ChildItem -LiteralPath $item.FullName -File -Recurse -Force | Sort-Object FullName)) {
                $shortcuts += [ordered]@{ Path = $file.FullName; Directory = $false; Hash = File-Hash $file.FullName }
            }
        }
    }
    # A portable Gesher makes no desktop shortcuts either (issue 519): every .lnk on
    # the user's desktop is preserved as it was.
    $desktop = [Environment]::GetFolderPath('Desktop')
    $desktopLinks = @()
    if ($desktop -and (Test-Path -LiteralPath $desktop)) {
        foreach ($file in @(Get-ChildItem -LiteralPath $desktop -Filter '*.lnk' -File -Force | Sort-Object FullName)) {
            $desktopLinks += [ordered]@{ Path = $file.FullName; Hash = File-Hash $file.FullName }
        }
    }
    return [ordered]@{
        Registry = $registry; UserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
        Shortcuts = $shortcuts; DesktopLinks = $desktopLinks
        UninstallerHash = File-Hash (Join-Path $InstalledRuntimeDirectory 'unins000.exe')
        UninstallerDataHash = File-Hash (Join-Path $InstalledRuntimeDirectory 'unins000.dat')
        InstalledVersionHash = File-Hash (Join-Path $InstalledRuntimeDirectory 'gesher-version.ini')
        NormalPackagesHash = File-Hash (Join-Path $env:LOCALAPPDATA 'Gesher\data\system\packages.list')
    } | ConvertTo-Json -Depth 30 -Compress
}
function Assert-HostUnchanged([string] $Phase) {
    $current = Host-State
    [IO.File]::WriteAllText((Join-Path $logs ($Phase + '-host.json')), $current)
    Assert-PortableRuntime ($current -ceq $script:hostBefore) "Portable $Phase changed the normal uninstall registration, PATH, associations, shortcuts or package registry. Evidence: $logs"
}
# $WhileRunning, when given, runs with the started process (e.g. to answer its
# dialogs); its result is left in $script:whileRunningResult, the exit code in
# $script:lastProcessExit.
function Invoke-CheckedProcess([string] $Executable, [string[]] $Arguments, [string] $Directory,
        [string] $Name, [int] $TimeoutSeconds = 180, [switch] $ExpectFailure, [scriptblock] $WhileRunning = $null) {
    $script:sequence++
    $log = Join-Path $logs ('{0:D2}-{1}' -f $script:sequence, $Name)
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $start.WorkingDirectory = $Directory
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    # Select the profile through the shipped marker detection. In particular do
    # not set GESHER_SHORTCUTS=0: that would hide broken portable integration.
    foreach ($variableName in @($start.Environment.Keys)) {
        if ($variableName -like 'ANDROID_*' -or $variableName -like 'GESHER_*') { $start.Environment.Remove($variableName) | Out-Null }
    }
    $start.Environment['GESHER_REQUIRE_BOOT_IMAGE'] = '1'
    $start.Environment['GESHER_APP_WINDOW'] = 'headless'
    $start.Environment['GESHER_LAUNCH_DETACH'] = '0'
    $start.Environment['GESHER_IP_GEOLOCATION'] = '0'
    $start.Environment['GESHER_LOCALES'] = 'en-US'
    $start.Environment['GESHER_PANEL_UI_LANGUAGE'] = 'en'
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $start
    $started = $false
    try {
        $started = $process.Start()
        if (-not $started) { throw "Could not start $Name" }
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        $script:whileRunningResult = $null
        if ($WhileRunning) { $script:whileRunningResult = & $WhileRunning $process }
        $finished = $process.WaitForExit($TimeoutSeconds * 1000)
        if (-not $finished) {
            $process.Kill($true)
            $null = $process.WaitForExit(5000)
        }
        $outputClosed = $output.Wait(5000); $errorsClosed = $errors.Wait(5000)
        if ($outputClosed) { [IO.File]::WriteAllText(($log + '.out.log'), $output.Result) }
        if ($errorsClosed) { [IO.File]::WriteAllText(($log + '.err.log'), $errors.Result) }
        if (-not $finished) { throw "$Name exceeded ${TimeoutSeconds}s; its own process tree was stopped. Evidence: $log" }
        if (-not $outputClosed -or -not $errorsClosed) { throw "$Name output pipes did not close within 5s" }
        $combined = $output.Result + "`n" + $errors.Result
        $script:lastProcessExit = $process.ExitCode
        Write-Host "$Name (exit $($process.ExitCode)):"
        Write-Host (($combined -split "`r?`n" | Select-Object -Last 15) -join "`n")
        if ($ExpectFailure) {
            Assert-PortableRuntime ($process.ExitCode -ne 0) "$Name reported success despite a failed portable finalization. Evidence: $log"
        } else {
            Assert-PortableRuntime ($process.ExitCode -eq 0) "$Name failed with exit $($process.ExitCode). Evidence: $log"
        }
        return $combined
    } finally {
        if ($started -and -not $process.HasExited) {
            $process.Kill($true); $null = $process.WaitForExit(5000)
        }
        $process.Dispose()
    }
}
function Invoke-App([string] $Runtime, [string[]] $Arguments, [string] $Name, [int] $TimeoutSeconds = 180) {
    return Invoke-CheckedProcess $powershell (@('-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
        '-File', (Join-Path $Runtime 'gesher-app.ps1')) + $Arguments) $Runtime $Name $TimeoutSeconds
}
function Owned-Processes([string] $Root) {
    foreach ($process in Get-Process) {
        try {
            $path = $process.Path
            if ($path -and (Is-InDirectory $path $Root) -and $process.StartTime.ToUniversalTime() -ge $testStarted) {
                [pscustomobject]@{ Id = $process.Id; Start = $process.StartTime.ToFileTimeUtc(); Path = $path }
            }
        } catch { } finally { $process.Dispose() }
    }
}
function Stop-OwnedProcesses([string] $Root) {
    foreach ($owned in @(Owned-Processes $Root)) {
        $process = Get-Process -Id $owned.Id -ErrorAction SilentlyContinue
        if (-not $process) { continue }
        try {
            # Recheck the OS creation time and exact executable before stopping
            # one PID. Never kill by a global name such as dalvikvm or binderd.
            if ($process.StartTime.ToFileTimeUtc() -eq $owned.Start -and $process.Path -ceq $owned.Path) {
                $process.Kill(); $null = $process.WaitForExit(5000)
            }
        } finally { $process.Dispose() }
    }
}
function Stop-PortableDevice([string] $Root, [string] $Runtime, [string] $Name) {
    $stateFile = Join-Path $Root 'Data\data\system\gesher-device'
    if (Test-Path -LiteralPath $stateFile -PathType Leaf) {
        $state = @{}
        foreach ($line in Get-Content -LiteralPath $stateFile) {
            if ($line -match '^(binderd|servicemanager|system_server)(\.start)?=(\d+)$') { $state[$Matches[1] + $Matches[2]] = $Matches[3] }
        }
        foreach ($role in 'binderd', 'servicemanager', 'system_server') {
            if (-not $state.ContainsKey($role)) { continue }
            $process = Get-Process -Id ([int]$state[$role]) -ErrorAction SilentlyContinue
            if (-not $process) { continue }
            try {
                if ($state.ContainsKey($role + '.start') -and $process.StartTime.ToFileTimeUtc() -ne [long]$state[$role + '.start']) { continue }
                Assert-PortableRuntime ((Is-InDirectory $process.Path $Root) -and $process.StartTime.ToUniversalTime() -ge $testStarted) 'Refusing to shut down a portable state file that names a foreign process'
            } finally { $process.Dispose() }
        }
    }
    $null = Invoke-App $Runtime @('shutdown') $Name 45
    Assert-PortableRuntime (@(Owned-Processes $Root).Count -eq 0) "Portable device still running after shutdown: $Root"
}
function Assert-Layout([string] $Root, [string] $Runtime) {
    foreach ($file in 'portable.marker', 'Gesher.exe') { Assert-PortableRuntime (Test-Path -LiteralPath (Join-Path $Root $file) -PathType Leaf) "Missing portable root file $file" }
    foreach ($file in 'gesher.ps1', 'gesher-app.ps1', 'gesher-runtime-files.ps1', 'gesher-update.ps1', 'gesher-version.ini', 'hello.dex', 'lifecycle.apk') {
        Assert-PortableRuntime (Test-Path -LiteralPath (Join-Path $Runtime $file) -PathType Leaf) "Missing packaged runtime file $file"
    }
    Assert-PortableRuntime (-not (Test-Path -LiteralPath (Join-Path $Runtime 'unins000.exe'))) 'Portable runtime contains a normal uninstaller'
    . (Join-Path $Runtime 'gesher-update.ps1')
    Assert-PortableRuntime ((Get-GesherInstalledVersion $Runtime) -ceq $Version) 'Portable runtime version differs from the tested installer'
    . (Join-Path $Runtime 'gesher-runtime-files.ps1')
    $savedProfile = $env:GESHER_PROFILE
    try {
        $env:GESHER_PROFILE = $null
        Assert-PortableRuntime ((Get-GesherProfileRoot $Runtime) -ceq (Join-Path $Root 'Data')) 'Portable preflight would use a foreign profile'
    } finally { $env:GESHER_PROFILE = $savedProfile }
    Assert-PortableRuntime ((File-Hash (Join-Path $Root 'Gesher.exe')) -ceq (File-Hash (Join-Path $Runtime 'gesher-launch.exe'))) 'Public portable launcher was not refreshed from its runtime'
}
function Invoke-Hello([string] $Runtime, [string] $Name) {
    $output = Invoke-CheckedProcess $powershell @('-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
        '-File', (Join-Path $Runtime 'gesher.ps1'), (Join-Path $Runtime 'hello.dex'), 'gesher.Hello') $Runtime $Name
    Assert-PortableRuntime ($output.Contains('Hello from ART on Windows') -and $output.Contains('gesher: boot image loaded: 3 spaces')) 'Portable Hello did not execute through its required boot image'
}
function Invoke-PortableSetup([string] $Runtime, [string] $Name, [switch] $ExpectFailure) {
    # Same flags supplied by the production updater, plus unattended CI/logging.
    $null = Invoke-CheckedProcess $InstallerPath @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/PORTABLE',
        ('/DIR=' + $Runtime), ('/LOG=' + (Join-Path $logs ($Name + '-setup.log')))) $work $Name 240 -ExpectFailure:$ExpectFailure
}
function Assert-PackageProfile([string] $Listing, [string] $Root, [string] $Runtime) {
    $match = [regex]::Match($Listing, '(?m)^package:com\.gesher\.lifecycle version .*? \(\d+\) (.+?), \d+ activities,')
    Assert-PortableRuntime $match.Success 'The real package list does not contain lifecycle.apk'
    $path = $match.Groups[1].Value.Replace('/', '\')
    if ($path -match '^\\mnt\\([a-zA-Z])\\(.*)$') { $path = $Matches[1] + ':\' + $Matches[2] }
    elseif ($path.StartsWith('\')) { $path = [IO.Path]::GetPathRoot($Runtime).TrimEnd('\') + $path }
    Assert-PortableRuntime (Test-Path -LiteralPath $path -PathType Leaf) "The package service reported a missing APK: $path"
    $long = [Text.StringBuilder]::new(32768)
    $length = [Gesher.PortableRuntimePaths]::GetLongPathName($path, $long, $long.Capacity)
    Assert-PortableRuntime ($length -gt 0 -and $length -lt $long.Capacity) 'Could not resolve the package service APK path'
    Assert-PortableRuntime (Is-InDirectory $long.ToString() (Join-Path $Root 'Data\data\app')) "The copied profile is still executing APK code from another profile: $long"
    $record = Join-Path $Root 'Data\data\data\com.gesher.lifecycle\gesher-shell\shortcut.properties'
    Assert-PortableRuntime (Test-Path -LiteralPath $record -PathType Leaf) 'Portable app metadata was not written in Data'
    $text = Get-Content -LiteralPath $record -Raw
    Assert-PortableRuntime ($text -match '(?m)^target=portable\s*$' -and $text -notmatch '(?m)^link=') 'Portable metadata retains authority over a host shortcut'
}

try {
    Assert-PortableRuntime (Test-Path -LiteralPath (Join-Path $InstalledRuntimeDirectory 'unins000.exe') -PathType Leaf) 'The existing standard installation is required for preservation checks'
    $registration = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($uninstallKey, $false)
    Assert-PortableRuntime ($null -ne $registration) 'Standard uninstall registration is missing before the portable check'
    $registration.Dispose()
    Assert-PortableRuntime (([Environment]::GetEnvironmentVariable('Path', 'User') -split ';') -contains $InstalledRuntimeDirectory) 'Standard runtime is not on the user PATH before the portable check'
    $script:hostBefore = Host-State
    [IO.File]::WriteAllText((Join-Path $logs 'before-host.json'), $script:hostBefore)
    if (-not ('Gesher.PortableRuntimePaths' -as [type])) { Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
using System.Text;
namespace Gesher {
  public static class PortableRuntimePaths {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern uint GetLongPathName(string path, StringBuilder output, int length);
  }
}
'@
    }
    # A real finalization failure must fail Setup, including in unattended mode.
    # A directory at the launcher destination is deterministic and needs no lock
    # on any executable, normal installation or user-owned file.
    $blocked = Join-Path $work 'blocked-launcher'
    $ownedRoots.Add($blocked)
    $null = New-Item -ItemType Directory -Path (Join-Path $blocked 'Gesher.exe'), (Join-Path $blocked 'Data') -Force
    [IO.File]::WriteAllText((Join-Path $blocked 'portable.marker'), '')
    $blockedData = Join-Path $blocked 'Data\portable-check.txt'
    [IO.File]::WriteAllText($blockedData, [guid]::NewGuid().ToString('N'))
    $blockedDataHash = File-Hash $blockedData
    Invoke-PortableSetup $blocked 'blocked-launcher-finalization' -ExpectFailure
    Assert-PortableRuntime (Test-Path -LiteralPath (Join-Path $blocked 'gesher-launch.exe') -PathType Leaf) 'Negative Setup case failed before extracting the runtime'
    Assert-PortableRuntime (Test-Path -LiteralPath (Join-Path $blocked 'Gesher.exe') -PathType Container) 'Negative Setup case did not retain its blocked launcher destination'
    Assert-PortableRuntime ((File-Hash $blockedData) -ceq $blockedDataHash) 'Failed portable finalization changed existing Data'
    Assert-HostUnchanged 'failed-portable-finalization'
    Stop-OwnedProcesses $blocked
    Assert-PortableRuntime (@(Owned-Processes $blocked).Count -eq 0) 'Failed portable finalization left a runtime process'
    Remove-Item -LiteralPath $blocked -Recurse -Force

    # Test the flat Setup layout first, then release its large runtime files
    # before extracting/copying two complete ZIP installations.
    $flat = Join-Path $work 'flat'
    $ownedRoots.Add($flat)
    Invoke-PortableSetup $flat 'flat-install'
    Assert-Layout $flat $flat
    Invoke-Hello $flat 'flat-hello'
    $flatData = Join-Path $flat 'Data\portable-check.txt'
    [IO.File]::WriteAllText($flatData, [guid]::NewGuid().ToString('N'))
    $flatDataHash = File-Hash $flatData
    [IO.File]::WriteAllText((Join-Path $flat 'Gesher.exe'), 'stale public launcher fixture; never executed')
    Invoke-PortableSetup $flat 'flat-update'
    Assert-Layout $flat $flat
    Assert-PortableRuntime ((File-Hash $flatData) -ceq $flatDataHash) 'Flat update changed user Data'
    Assert-HostUnchanged 'flat-install-and-update'
    Stop-OwnedProcesses $flat
    Assert-PortableRuntime (@(Owned-Processes $flat).Count -eq 0) 'Flat runtime still owns a process'
    Remove-Item -LiteralPath $flat -Recurse -Force

    $extraction = Join-Path $work 'zip'
    Expand-Archive -LiteralPath $ZipPath -DestinationPath $extraction
    $original = Join-Path $extraction 'GesherPortable'
    $originalRuntime = Join-Path $original 'App'
    $ownedRoots.Add($original)
    Assert-Layout $original $originalRuntime
    Invoke-Hello $originalRuntime 'zip-hello'
    $null = Invoke-App $originalRuntime @('install', (Join-Path $originalRuntime 'lifecycle.apk')) 'zip-install-lifecycle'
    # A desktop shortcut asked for in portable mode: none, and the output says why
    # (Assert-HostUnchanged below: no .lnk on the desktop changed or appeared).
    $desktopInstall = Invoke-App $originalRuntime @('install', (Join-Path $originalRuntime 'lifecycle.apk'), '--desktop-shortcut') 'zip-install-desktop-shortcut'
    Assert-PortableRuntime ($desktopInstall -match 'no desktop shortcut for com\.gesher\.lifecycle') 'Portable install --desktop-shortcut did not say why it made no desktop shortcut'
    $lifecycleRecordText = Get-Content -LiteralPath (Join-Path $original 'Data\data\data\com.gesher.lifecycle\gesher-shell\shortcut.properties') -Raw
    Assert-PortableRuntime ($lifecycleRecordText -notmatch '(?m)^desktop_link=') 'Portable install recorded a desktop shortcut'
    $launched = Invoke-App $originalRuntime @('launch', 'com.gesher.lifecycle', '--headless', '--wait') 'zip-launch-lifecycle'
    Assert-PortableRuntime ($launched.Contains('LifecycleActivity onResume') -and $launched.Contains('gesher-app: done')) 'Portable lifecycle did not reach onResume through the real Android runtime'
    if ($ComponentsApk) {
        $null = Invoke-App $originalRuntime @('install', $ComponentsApk) 'zip-install-components'
    } else { Write-Host 'LIMIT: no ComponentsApk supplied; custom URI protocol candidate was not exercised.' }
    $null = Invoke-App $originalRuntime @('sync-shortcuts') 'zip-sync-shortcuts'
    $listing = Invoke-App $originalRuntime @('list') 'zip-list'
    Assert-PackageProfile $listing $original $originalRuntime
    Assert-HostUnchanged 'zip-app-install'
    $userFile = Join-Path $original 'Data\data\data\com.gesher.lifecycle\portable-check.txt'
    [IO.File]::WriteAllText($userFile, [guid]::NewGuid().ToString('N'))
    $userHash = File-Hash $userFile
    Stop-PortableDevice $original $originalRuntime 'zip-shutdown-before-copy'
    $originalRegistryHash = File-Hash (Join-Path $original 'Data\data\system\packages.list')

    # Keep the original populated code tree present. A missing-old-path-only
    # relocation fallback would otherwise pass without providing copy isolation.
    $copy = Join-Path $work 'populated-copy'
    $ownedRoots.Add($copy)
    Copy-Item -LiteralPath $original -Destination $copy -Recurse
    $copyRuntime = Join-Path $copy 'App'
    Assert-PortableRuntime (Test-Path -LiteralPath (Join-Path $original 'Data\data\app') -PathType Container) 'Original populated profile vanished before copied-profile validation'
    Assert-Layout $copy $copyRuntime
    $copiedLaunch = Invoke-App $copyRuntime @('launch', 'com.gesher.lifecycle', '--headless', '--wait') 'copy-launch-lifecycle'
    Assert-PortableRuntime ($copiedLaunch.Contains('LifecycleActivity onResume')) 'Copied profile could not execute its installed lifecycle activity'
    $copyList = Invoke-CheckedProcess (Join-Path $copy 'Gesher.exe') @('list') $copy 'copy-public-launcher-list'
    Assert-PackageProfile $copyList $copy $copyRuntime
    Assert-PortableRuntime ((File-Hash (Join-Path $copy 'Data\data\data\com.gesher.lifecycle\portable-check.txt')) -ceq $userHash) 'Copied profile lost private app data'
    Assert-PortableRuntime ((File-Hash (Join-Path $original 'Data\data\system\packages.list')) -ceq $originalRegistryHash -and (File-Hash $userFile) -ceq $userHash) 'Running the copied profile mutated original package data'
    Stop-PortableDevice $copy $copyRuntime 'copy-shutdown-before-update'
    [IO.File]::WriteAllText((Join-Path $copy 'Gesher.exe'), 'stale public launcher fixture; never executed')
    Invoke-PortableSetup $copyRuntime 'app-layout-update'
    Assert-Layout $copy $copyRuntime
    Assert-PortableRuntime (-not (Test-Path -LiteralPath (Join-Path $copyRuntime 'portable.marker'))) 'App update created a second portable root inside App'
    Assert-PortableRuntime ((File-Hash (Join-Path $copy 'Data\data\data\com.gesher.lifecycle\portable-check.txt')) -ceq $userHash) 'App-layout update changed private app data'
    $afterUpdate = Invoke-CheckedProcess (Join-Path $copy 'Gesher.exe') @('list') $copy 'updated-public-launcher-list'
    Assert-PackageProfile $afterUpdate $copy $copyRuntime
    if ($ComponentsApk) {
        $null = Invoke-App $copyRuntime @('uninstall', 'com.gesher.components') 'copy-remove-components'
        Assert-HostUnchanged 'portable-protocol-removal'
    }
    # An APK dropped on the portable Gesher.exe (Explorer's bare form): the
    # confirmation dialog without a desktop shortcut box; Install, then Done,
    # installs it in the portable profile and launches nothing; the same file
    # again opens the installed package with no dialog.
    . (Join-Path $PSScriptRoot 'InstallConfirmDialog.ps1')
    $null = Invoke-App $copyRuntime @('uninstall', 'com.gesher.lifecycle') 'copy-remove-lifecycle'
    $dropped = Join-Path $copyRuntime 'lifecycle.apk'
    $cancelled = Invoke-CheckedProcess (Join-Path $copy 'Gesher.exe') @($dropped) $copy 'copy-dropped-apk-cancel' 600 -ExpectFailure -WhileRunning {
        param($process)
        Invoke-GesherInstallConfirm -ProcessId $process.Id -Process $process -Answer Cancel -ExpectDesktopBox $false
    }
    Assert-PortableRuntime (-not $script:whileRunningResult.Problems) "Portable dropped APK, Cancel: $($script:whileRunningResult.Problems -join '; ')"
    Assert-PortableRuntime ($script:lastProcessExit -eq 1223 -and $cancelled -notmatch 'installed com\.gesher\.lifecycle') "Portable dropped APK, Cancel: exit $($script:lastProcessExit), not 1223, or it installed"
    $installed = Invoke-CheckedProcess (Join-Path $copy 'Gesher.exe') @($dropped) $copy 'copy-dropped-apk-install' 600 -WhileRunning {
        param($process)
        Invoke-GesherInstallConfirm -ProcessId $process.Id -Process $process -Answer Install -ExpectDesktopBox $false -Done Done
    }
    Assert-PortableRuntime (-not $script:whileRunningResult.Problems) "Portable dropped APK, Install: $($script:whileRunningResult.Problems -join '; ')"
    Assert-PortableRuntime ($installed -match 'installed com\.gesher\.lifecycle' -and $installed -notmatch 'gesher-app: done' -and $installed -notmatch 'LifecycleActivity') 'Portable dropped APK: Install did not install it, or launched it without Open'
    $reopened = Invoke-CheckedProcess (Join-Path $copy 'Gesher.exe') @($dropped) $copy 'copy-dropped-apk-same-version' 600 -WhileRunning {
        param($process)
        @(Watch-GesherNoInstallDialog $process 600)
    }
    Assert-PortableRuntime (@($script:whileRunningResult | Where-Object { $_ }).Count -eq 0) "Portable dropped APK of the installed version showed a dialog: $($script:whileRunningResult -join ', ')"
    Assert-PortableRuntime ($reopened -match 'launching the installed package com\.gesher\.lifecycle' -and $reopened -match 'LifecycleActivity onResume') 'Portable dropped APK of the installed version did not launch the installed package'
    $afterDrop = Invoke-CheckedProcess (Join-Path $copy 'Gesher.exe') @('list') $copy 'copy-dropped-apk-list'
    Assert-PackageProfile $afterDrop $copy $copyRuntime
    Assert-HostUnchanged 'portable-dropped-apk'
    Stop-PortableDevice $copy $copyRuntime 'copy-final-shutdown'
    Assert-HostUnchanged 'all-portable-operations'
    $receipt = [ordered]@{
        Passed = $true; Checks = $script:checks; Version = $Version
        ZipSha256 = File-Hash $ZipPath; InstallerSha256 = File-Hash $InstallerPath
        ComponentsApkSha256 = $(if ($ComponentsApk) { File-Hash $ComponentsApk })
        ProtocolCandidateExercised = [bool]$ComponentsApk
        Scope = 'Actual Windows/ART Hello, lifecycle install/launch/list, populated-copy isolation, an APK dropped on Gesher.exe (confirmation without a desktop shortcut box, Cancel, Install, the installed version opened directly), flat/App Setup refresh and preservation of normal uninstall/PATH/associations/shortcuts/desktop links.'
        Limits = 'Same artifact reinstalls exercise update layout; no network update discovery or physical USB removal. Notifications can still register AppUserModelIds.'
    }
} catch {
    $failure = $_
    [IO.File]::WriteAllText((Join-Path $logs 'failure.txt'), ($_ | Out-String))
} finally {
    foreach ($root in $ownedRoots) {
        try {
            Stop-OwnedProcesses $root
            if (@(Owned-Processes $root).Count) { throw "Runtime processes remain under $root" }
            if (Test-Path -LiteralPath $root) {
                if (-not (Is-InDirectory $root $work)) { throw 'Refusing cleanup outside the unique test workspace' }
                $profileLogs = Join-Path $root 'Data\logs'
                if (Test-Path -LiteralPath $profileLogs) {
                    Copy-Item -LiteralPath $profileLogs -Destination (Join-Path $logs ((Split-Path $root -Leaf) + '-device')) -Recurse -Force
                }
                Remove-Item -LiteralPath $root -Recurse -Force
            }
        } catch { $cleanupFailures.Add($_.Exception.Message); Write-Warning $_.Exception.Message }
    }
    Write-Host "Portable runtime evidence: $logs"
}
if ($failure) { throw $failure }
if ($cleanupFailures.Count) { throw ('Portable cleanup failed: ' + ($cleanupFailures -join '; ')) }
[IO.File]::WriteAllText((Join-Path $logs 'validation.json'), ($receipt | ConvertTo-Json -Depth 5))
Write-Host "PASS: portable runtime ($script:checks checks). Evidence: $logs"
