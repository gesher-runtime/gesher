# Actual WebView2 login-compatibility fixtures. All app requests use local HTTP servers.
# Does not open Google or read an existing runtime/browser profile.
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Dir,
    [string] $Apk,
    [string] $EvidenceDir,
    [switch] $SkipWorkers,
    [ValidateSet('bridge-cookie-worker', 'fragment-history-callbacks', 'worker-bridge-boundaries', 'inflight-view-destruction')]
    [string] $OnlyCase
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$Dir = (Resolve-Path -LiteralPath $Dir).Path
if (-not $Apk) { $Apk = Join-Path $Dir 'bridgecookies.apk' }
$Apk = (Resolve-Path -LiteralPath $Apk).Path
$launcher = Join-Path $Dir 'gesher-app.ps1'
if (-not (Test-Path -LiteralPath $launcher)) { throw 'The runtime must include its normal gesher-app.ps1 launcher.' }
if (-not $EvidenceDir) {
    $temporary = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
    $EvidenceDir = Join-Path $temporary 'gesher-webview-login'
}
$run = Join-Path ([IO.Path]::GetFullPath($EvidenceDir)) ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $run | Out-Null
Write-Host "INFO  local WebView login fixture evidence: $run"
# Keep Chromium's nested cache paths below legacy Windows path limits even when
# the caller chooses a deeply nested evidence directory. All data is fresh and
# retained; the exact location is recorded with the evidence.
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$profileRoot = Join-Path (Join-Path $workspace 'out/wv-p') ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profileRoot | Out-Null
[IO.File]::WriteAllText((Join-Path $run 'profile-root.txt'), $profileRoot)
$artifacts = @('dalvikvm.exe', 'gesher-app.jar', 'libgesher_webview_bridge.so') | ForEach-Object {
    $path = Join-Path $Dir $_
    if (Test-Path -LiteralPath $path) {
        @{ File = $_; Sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
    }
}
$artifacts += @{ File = 'bridgecookies.apk'; Sha256 = (Get-FileHash -LiteralPath $Apk -Algorithm SHA256).Hash }
[IO.File]::WriteAllText((Join-Path $run 'runtime-artifacts.json'), (ConvertTo-Json -InputObject @($artifacts) -Depth 3))
$environment = @{}
$names = @('LOCALAPPDATA', 'APPDATA', 'GESHER_LOGS', 'GESHER_MICROG_SPOOF', 'GESHER_SHORTCUTS',
    'GESHER_IDLE_TIMEOUT', 'GESHER_IP_GEOLOCATION', 'GESHER_WEBVIEW_DEBUG', 'GESHER_WEBVIEW_ARGS', 'GESHER_BINDER', 'GESHER_BINDER_UID', 'GESHER_UID')
foreach ($name in $names) { $environment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$shellExe = (Get-Process -Id $PID).Path
$failed = $false
$checks = 0
$script:sequence = 0

function Quote([string] $Value) {
    if ($Value -eq '') { return '""' }
    if ($Value -notmatch '[\s"]') { return $Value }
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    '"' + [regex]::Replace($escaped, '(\\+)$', '$1$1') + '"'
}
function ReadLog([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    $reader = New-Object IO.StreamReader($stream)
    try { $reader.ReadToEnd() } finally { $reader.Dispose(); $stream.Dispose() }
}
function RunLauncher([string] $Label, [string[]] $Arguments, [int] $TimeoutSeconds = 90) {
    $script:sequence++
    $base = Join-Path $run ('{0:D2}-{1}' -f $script:sequence, $Label)
    $command = (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $launcher) + $Arguments |
        ForEach-Object { Quote $_ }) -join ' '
    $process = Start-Process -FilePath $shellExe -ArgumentList $command -WorkingDirectory $Dir -WindowStyle Hidden `
        -PassThru -RedirectStandardOutput "$base.out.log" -RedirectStandardError "$base.err.log"
    $handle = $process.Handle
    try {
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill(); throw "$Label timed out; see $base.*.log"
        }
        if ($process.ExitCode -ne 0) { throw "$Label exited $($process.ExitCode); see $base.*.log" }
    } finally { $process.Dispose() }
}
try {
    $env:GESHER_MICROG_SPOOF = '1'
    $env:GESHER_SHORTCUTS = '0'
    $env:GESHER_IDLE_TIMEOUT = '180'
    $env:GESHER_IP_GEOLOCATION = '0'
    $env:GESHER_WEBVIEW_DEBUG = $null
    $env:GESHER_WEBVIEW_ARGS = '--disable-background-networking --disable-component-update --no-first-run --disable-sync'
    [IO.File]::WriteAllText((Join-Path $run 'browser-arguments.txt'), $env:GESHER_WEBVIEW_ARGS)
    $env:GESHER_BINDER = $null
    $env:GESHER_BINDER_UID = $null
    $env:GESHER_UID = $null
    $cases = @(
        @{ Name = 'bridge-cookie-worker'; Activity = '.BridgeCookiesActivity'; Tag = 'bridgecookies' },
        @{ Name = 'fragment-history-callbacks'; Activity = '.NavigationActivity'; Tag = 'bridge-navigation' }
    )
    if (-not $SkipWorkers) { $cases += @{ Name = 'worker-bridge-boundaries'; Activity = '.WorkerActivity'; Tag = 'bridgeworkers' } }
    $cases += @{ Name = 'inflight-view-destruction'; Activity = '.InflightDestroyActivity'; Tag = 'bridge-inflight' }
    if ($OnlyCase) { $cases = @($cases | Where-Object { $_.Name -eq $OnlyCase }) }
    if ($cases.Count -eq 0) { throw 'The requested case was excluded by the selected switches.' }
    [IO.File]::WriteAllLines((Join-Path $run 'selected-cases.txt'), [string[]]($cases | ForEach-Object { $_.Name }))
    if ($OnlyCase) { Write-Host "INFO  focused diagnostic case only: $OnlyCase" }
    $caseIndex = 0
    foreach ($case in $cases) {
        # The app bootstrap may select its default WebView folder before Activity
        # code runs. Isolate Android and browser data per case, not just per run.
        $caseRoot = Join-Path $run $case.Name
        $caseIndex++
        $caseProfile = Join-Path $profileRoot ([string]$caseIndex)
        $env:LOCALAPPDATA = $caseProfile
        $env:APPDATA = Join-Path $caseProfile 'roaming'
        New-Item -ItemType Directory -Force $env:APPDATA | Out-Null
        $env:GESHER_LOGS = ((Join-Path $caseRoot 'logs') -replace '\\', '/') -replace '^[A-Za-z]:', ''
        try {
            RunLauncher "$($case.Name)-install" @('install', $Apk)
            RunLauncher "$($case.Name)-direct-proxy" @('proxy', 'com.gesher.bridgecookies', 'direct')
            $log = Join-Path $caseRoot 'logs/com.gesher.bridgecookies.log'
            RunLauncher $case.Name @('launch', 'com.gesher.bridgecookies', '--activity', $case.Activity, '--hidden-window', '--keep-task')
            $text = ReadLog $log
            [IO.File]::WriteAllText((Join-Path $run "$($case.Name).activity.log"), $text)
            if ($text.Contains("$($case.Tag): FAIL") -or -not $text.Contains("$($case.Tag): all passed")) {
                throw "$($case.Name) did not pass; see its activity log in $run"
            }
            $checks++
            Write-Host "PASS  $($case.Name)"
            $text -split "`r?`n" | Where-Object { $_.StartsWith("$($case.Tag): PASS") } | ForEach-Object { Write-Host $_ }
        } finally {
            RunLauncher "$($case.Name)-shutdown" @('shutdown') 45
        }
    }
    if ($SkipWorkers) { Write-Host 'SKIP  worker source-boundary fixture (explicit -SkipWorkers)' }
} catch {
    $failed = $true
    [IO.File]::WriteAllText((Join-Path $run 'failure.txt'), $_.Exception.ToString())
    Write-Host "FAIL  $($_.Exception.Message)"
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $environment[$name], 'Process') }
    [IO.File]::WriteAllText((Join-Path $run 'result.txt'), "passed=$checks`nfailed=$failed`n")
    Write-Host "INFO  $checks local WebView fixture(s) passed; evidence retained: $run"
}
if ($failed) { exit 1 }
