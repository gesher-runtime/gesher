# Offline actual-Binder installer transport regression. Never commits an APK or opens consent UI.
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Dir,
    [string] $ServicesDex,
    [string] $SigningFixtures,
    [string] $EvidenceDir,
    [switch] $RequireBootImages
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$Dir = (Resolve-Path -LiteralPath $Dir).Path
if ($Dir -notmatch '^[A-Za-z]:\\') { throw 'Dir must be a local Windows runtime directory.' }
if (-not $ServicesDex) { $ServicesDex = Join-Path $Dir 'services-tests.dex' }
if (-not $SigningFixtures) { $SigningFixtures = Join-Path $PSScriptRoot '../dex-services/fixtures' }
$ServicesDex = (Resolve-Path -LiteralPath $ServicesDex).Path
$apk = (Resolve-Path -LiteralPath (Join-Path $SigningFixtures 'v2only.apk')).Path
foreach ($name in 'dalvikvm.exe','binderd.exe','servicemanager.exe','core-oj.jar','core-libart.jar',
        'core-icu4j.jar','framework.jar','gesher-app.jar','framework-res.apk','build.prop',
        'framework/x86_64/boot.art','libgesher_account_crypto.so','libgesher_install_pipe.so','install_pipe_test.exe') {
    if (-not (Test-Path -LiteralPath (Join-Path $Dir $name) -PathType Leaf)) { throw "Missing installer fixture input: $name" }
}
$icu = @(Get-ChildItem -LiteralPath $Dir -Filter 'icudt*l.dat' -File)
if ($icu.Count -ne 1) { throw 'Exactly one ICU data file is required.' }
if (-not $EvidenceDir) {
    $temporary = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
    $EvidenceDir = Join-Path $temporary 'gesher-package-installer'
}
$run = Join-Path ([IO.Path]::GetFullPath($EvidenceDir)) ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$profile = Join-Path $run 'profile/Gesher'
$logs = Join-Path $run 'device-logs'
$environmentNames = @('LOCALAPPDATA','APPDATA','ANDROID_ROOT','ANDROID_DATA','ANDROID_ART_ROOT',
    'ANDROID_I18N_ROOT','ANDROID_TZDATA_ROOT','GESHER_MICROG_SPOOF','GESHER_SHORTCUTS',
    'GESHER_IDLE_TIMEOUT','GESHER_LOGS','GESHER_UID','GESHER_BINDER_UID','GESHER_BINDER',
    'GESHER_APP_WINDOW','GESHER_IP_GEOLOCATION','GESHER_REQUIRE_BOOT_IMAGE')
$saved = @{}
foreach ($name in $environmentNames) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$originalLocation = Get-Location
$known = @{}
$failed = $false
$sequence = 0
$vm = @()
function AndroidPath([string] $Path) {
    $value = [IO.Path]::GetFullPath($Path) -replace '\\','/'
    if ($value -notmatch '^([A-Za-z]):(.*)$') { throw 'Unsupported Android path.' }
    if ($Matches[1] -ieq $Dir.Substring(0,1)) { return $Matches[2] }
    return "/mnt/$($Matches[1].ToLowerInvariant())$($Matches[2])"
}
function QuoteArgument([string] $Value) {
    if ($Value -eq '') { return '""' }
    if ($Value -notmatch '[\s"]') { return $Value }
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}
function ReadSharedLog([string] $Path) {
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
function InvokeFixture([string] $Label, [string] $Executable, [string[]] $Arguments, [string] $Marker = '') {
    $script:sequence++
    $base = Join-Path $run ('{0:D2}-{1}' -f $script:sequence, $Label)
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.Arguments = ($Arguments | ForEach-Object { QuoteArgument $_ }) -join ' '
    $start.WorkingDirectory = $Dir
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw "Cannot start $Label" }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(60000)) { $process.Kill(); throw "$Label timed out" }
        $process.WaitForExit()
        $text = $stdout.Result
        [IO.File]::WriteAllText("$base.out.log", $text)
        [IO.File]::WriteAllText("$base.err.log", $stderr.Result)
        if ($RequireBootImages -and [IO.Path]::GetFileName($Executable) -eq 'dalvikvm.exe') {
            $imageText = $text + $stderr.Result
            if (-not $imageText.Contains('gesher: boot image loaded: 5 spaces (boot.art boot-core-libart.art boot-core-icu4j.art boot-framework.art boot-gesher-app.art)') -or
                    $imageText.Contains('imageless')) { throw "$Label did not use the complete boot image chain" }
        }
        if ($process.ExitCode -ne 0 -or ($Marker -and -not $text.Contains($Marker))) {
            throw "$Label failed; see $base.*.log"
        }
    } finally { $process.Dispose() }
}
function RememberDevice {
    $state = @{}
    $file = Join-Path $profile 'data/system/gesher-device'
    if (Test-Path -LiteralPath $file) {
        foreach ($line in Get-Content -LiteralPath $file) {
            if ($line -match '^\s*[#!]' -or $line -notmatch '=') { continue }
            $key, $value = $line -split '=',2
            $state[$key.Trim()] = $value.Trim()
        }
    }
    foreach ($name in 'system_server','servicemanager','binderd') {
        if (-not $state.ContainsKey($name) -or -not $state.ContainsKey("$name.start")) { continue }
        $process = Get-Process -Id ([int]$state[$name]) -ErrorAction SilentlyContinue
        if ($process) {
            try {
                $began = $process.StartTime.ToFileTimeUtc()
                if ($began -eq [long]$state["$name.start"]) { $known[$process.Id] = $began }
            } catch { }
        }
    }
    if ($state.ContainsKey('device')) { $env:GESHER_BINDER = $state['device'] }
}
try {
    Set-Location -LiteralPath $Dir
    foreach ($sub in 'system/framework','system/etc/permissions','data','art','i18n/etc/icu') {
        New-Item -ItemType Directory -Force (Join-Path $profile $sub) | Out-Null
    }
    New-Item -ItemType Directory -Force $logs | Out-Null
    Copy-Item -LiteralPath "$Dir/framework-res.apk" -Destination "$profile/system/framework/framework-res.apk"
    Copy-Item -LiteralPath "$Dir/build.prop" -Destination "$profile/system/build.prop"
    Copy-Item -LiteralPath $icu[0].FullName -Destination "$profile/i18n/etc/icu/$($icu[0].Name)"
    foreach ($library in 'org.apache.http.legacy','com.android.location.provider') {
        if (Test-Path -LiteralPath "$Dir/$library.jar") {
            Copy-Item -LiteralPath "$Dir/$library.jar" -Destination "$profile/system/framework/$library.jar"
            $declaration = "$Dir/etc/permissions/$library.xml"
            if (-not (Test-Path -LiteralPath $declaration)) {
                $declaration = Join-Path $PSScriptRoot "../../tools/package/etc/permissions/$library.xml"
            }
            Copy-Item -LiteralPath $declaration -Destination "$profile/system/etc/permissions/$library.xml"
        }
    }
    $env:LOCALAPPDATA = Join-Path $run 'profile'
    $env:APPDATA = Join-Path $run 'profile/roaming'
    $env:ANDROID_ROOT = AndroidPath "$profile/system"
    $env:ANDROID_DATA = AndroidPath "$profile/data"
    $env:ANDROID_ART_ROOT = AndroidPath "$profile/art"
    $env:ANDROID_I18N_ROOT = AndroidPath "$profile/i18n"
    $env:ANDROID_TZDATA_ROOT = AndroidPath "$Dir/tzdata"
    $env:GESHER_LOGS = AndroidPath $logs
    $env:GESHER_MICROG_SPOOF = '1'
    $env:GESHER_SHORTCUTS = '0'
    $env:GESHER_IDLE_TIMEOUT = '3600'
    $env:GESHER_APP_WINDOW = 'headless'
    $env:GESHER_IP_GEOLOCATION = '0'
    if ($RequireBootImages) { $env:GESHER_REQUIRE_BOOT_IMAGE = '1' }
    $env:GESHER_BINDER = $null
    $env:GESHER_UID = $null
    $env:GESHER_BINDER_UID = $null
    $bcp = (('core-oj','core-libart','core-icu4j','framework','gesher-app') | ForEach-Object { AndroidPath "$Dir/$_.jar" }) -join ':'
    $vm = @("-Xbootclasspath:$bcp", "-Ximage:$(AndroidPath "$Dir/framework/boot.art")", '-Xint','-Xusejit:false')
    InvokeFixture 'native-core' "$Dir/install_pipe_test.exe" @() 'install_pipe_test all passed'
    InvokeFixture 'start-device' "$Dir/dalvikvm.exe" ($vm + @('com.gesher.app.Launcher','start'))
    RememberDevice
    if (-not $env:GESHER_BINDER) { throw 'Disposable installer device was not recorded.' }
    $env:GESHER_UID = '2000'; $env:GESHER_BINDER_UID = '2000'
    InvokeFixture 'framework-serial' "$Dir/dalvikvm.exe" ($vm + @('-cp', (AndroidPath $ServicesDex),
        'gesher.services.DeviceSerialIpcTest')) 'DeviceSerialIpcTest: remote Build.getSerial equals Build.UNKNOWN'
    $test = $vm + @('-cp', (AndroidPath $ServicesDex), 'gesher.services.PackageInstallerIpcTest')
    InvokeFixture 'remote-stream' "$Dir/dalvikvm.exe" ($test + @('stream', (AndroidPath $apk))) 'PackageInstallerIpcTest all passed'
    InvokeFixture 'retained-client-cancellation' "$Dir/dalvikvm.exe" ($test + 'cancel') 'PackageInstallerIpcTest all passed'
    $env:GESHER_UID = '12345'; $env:GESHER_BINDER_UID = '12345'
    InvokeFixture 'untrusted-client' "$Dir/dalvikvm.exe" ($test + 'untrusted') 'PackageInstallerIpcTest all passed'
    if ($RequireBootImages) {
        $serverImages = ReadSharedLog (Join-Path $logs 'system_server.log')
        if (-not $serverImages.Contains('gesher: boot image loaded: 5 spaces (boot.art boot-core-libart.art boot-core-icu4j.art boot-framework.art boot-gesher-app.art)') -or
                $serverImages.Contains('imageless')) { throw 'Installer server did not use the complete boot image chain' }
    }
    Write-Host 'PASS  package installer real Binder stream, cancellation and role integration complete'
} catch {
    $failed = $true
    [IO.File]::WriteAllText((Join-Path $run 'failure.txt'), $_.Exception.ToString())
    Write-Host "FAIL  $($_.Exception.Message)"
} finally {
    $env:GESHER_UID = $null; $env:GESHER_BINDER_UID = $null
    try {
        RememberDevice
        if ($env:GESHER_BINDER -and $vm.Count) {
            InvokeFixture 'shutdown-device' "$Dir/dalvikvm.exe" ($vm + @('com.gesher.app.Launcher','shutdown'))
        }
    } catch { }
    foreach ($entry in $known.GetEnumerator()) {
        $process = Get-Process -Id ([int]$entry.Key) -ErrorAction SilentlyContinue
        if ($process) {
            try {
                if ($process.StartTime.ToFileTimeUtc() -eq $entry.Value) { Stop-Process -Id $process.Id -Force }
            } catch { }
        }
    }
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    Set-Location $originalLocation
    [IO.File]::WriteAllText((Join-Path $run 'result.txt'), "failed=$failed`n")
    Write-Host "INFO  installer evidence retained at $run"
}
if ($failed) { exit 1 }
