# Offline microG security integration on the actual Windows ART runtime.
# No Google APK is installed or executed, and no actual user's consent dialog is driven.
# Native chooser controls use synthetic labels on owned private desktops, never SwitchDesktop.
# Optional GmsCoreApk is parsed only by offline metadata/policy tests.
# Example (a complete assembled runtime, with the two test APKs and services-tests.dex):
#   powershell -File tests/windows/run-microg-security.ps1 -Dir out/windows-x64
# Add -GmsCoreApk <official.apk> -RequireMetadataSuites for the metadata-dependent suites.
# This exercises Gesher's app identities/service policy, not a Windows native-code sandbox
# or complete Android/microG API parity. Logs and the fresh fixture profile are retained.
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Dir,
    [string] $ServicesDex,
    [string] $DevsvcApk,
    [string] $ProbeApk,
    [string] $SigningFixtures,
    [string] $GmsCoreApk,
    [switch] $RequireMetadataSuites,
    [string] $EvidenceDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
if (-not $SigningFixtures) { $SigningFixtures = Join-Path $PSScriptRoot '../dex-services/fixtures' }

function ExistingPath([string] $Path) { (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path }
$Dir = ExistingPath $Dir
if ($Dir -notmatch '^[A-Za-z]:\\') { throw 'Dir must be a local Windows runtime directory.' }
if (-not $ServicesDex) { $ServicesDex = Join-Path $Dir 'services-tests.dex' }
if (-not $DevsvcApk) { $DevsvcApk = Join-Path $Dir 'devsvc.apk' }
if (-not $ProbeApk) { $ProbeApk = Join-Path $Dir 'microgprobe.apk' }
$ServicesDex = ExistingPath $ServicesDex
$DevsvcApk = ExistingPath $DevsvcApk
$ProbeApk = ExistingPath $ProbeApk
$SigningFixtures = ExistingPath $SigningFixtures
if ($GmsCoreApk) { $GmsCoreApk = ExistingPath $GmsCoreApk }
if ($RequireMetadataSuites -and -not $GmsCoreApk) {
    throw '-RequireMetadataSuites requires a local official -GmsCoreApk (metadata only).'
}
foreach ($file in 'dalvikvm.exe', 'binderd.exe', 'servicemanager.exe',
        'core-oj.jar', 'core-libart.jar', 'core-icu4j.jar', 'framework.jar',
        'gesher-app.jar', 'framework-res.apk', 'build.prop', 'framework/x86_64/boot.art',
        'libgesher_account_crypto.so', 'libgesher_install_pipe.so') {
    if (-not (Test-Path -LiteralPath (Join-Path $Dir $file) -PathType Leaf)) {
        throw "Missing runtime input: $file"
    }
}
$icu = @(Get-ChildItem -LiteralPath $Dir -Filter 'icudt*l.dat' -File)
if ($icu.Count -ne 1) { throw 'The runtime must contain exactly one icudt*l.dat.' }
foreach ($file in 'v2only.apk', 'rotated.apk', 'unsigned.apk', 'process-metadata.apk',
        '../identity-fixtures/old.apk', '../identity-fixtures/rotated-no-data.apk',
        '../identity-fixtures/rotated-no-auth.apk') {
    if (-not (Test-Path -LiteralPath (Join-Path $SigningFixtures $file) -PathType Leaf)) {
        throw "Missing committed signing fixture: $file"
    }
}
if (-not $EvidenceDir) {
    $temporaryRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
    $EvidenceDir = Join-Path $temporaryRoot 'gesher-microg-security'
}
$EvidenceDir = [IO.Path]::GetFullPath($EvidenceDir)
# Always create a unique child; never reuse/delete a caller's profile or runtime.
$run = Join-Path $EvidenceDir ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
Write-Host "INFO  offline microG security evidence: $run"

$originalLocation = Get-Location
$environmentNames = @('LOCALAPPDATA', 'APPDATA', 'ANDROID_ROOT', 'ANDROID_DATA',
    'ANDROID_ART_ROOT', 'ANDROID_I18N_ROOT', 'ANDROID_TZDATA_ROOT', 'GESHER_MICROG_SPOOF',
    'GESHER_SHORTCUTS', 'GESHER_IDLE_TIMEOUT', 'GESHER_LOGS', 'GESHER_UID',
    'GESHER_BINDER_UID', 'GESHER_BINDER', 'GESHER_APP_WINDOW', 'GESHER_IP_GEOLOCATION')
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$script:sequence = 0
$script:checks = 0
$script:knownProcesses = @{}
$script:deviceRoot = $null
$script:deviceLogs = $null
$script:failed = $false
$script:vm = @()

function AndroidPath([string] $Path) {
    $absolute = [IO.Path]::GetFullPath($Path) -replace '\\', '/'
    if ($absolute -notmatch '^([A-Za-z]):(.*)$') { throw "Unsupported Android path: $Path" }
    if ($Matches[1] -ieq $Dir.Substring(0, 1)) { return $Matches[2] }
    return "/mnt/$($Matches[1].ToLowerInvariant())$($Matches[2])"
}

function QuoteArgument([string] $Value) {
    if ($Value -eq '') { return '""' }
    if ($Value -notmatch '[\s"]') { return $Value }
    # CommandLineToArgvW/CRT quoting, including backslashes before quotes and at the end.
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

function ReadLog([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    $stream = $null
    $reader = $null
    try {
        $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
        return $reader.ReadToEnd()
    } catch [IO.IOException] { return '' }
    finally {
        if ($reader) { $reader.Dispose() }
        elseif ($stream) { $stream.Dispose() }
    }
}

function InvokeVm([string] $Label, [string[]] $Arguments, [int] $TimeoutSeconds = 120) {
    $script:sequence++
    $base = Join-Path $run ('{0:D3}-{1}' -f $script:sequence, ($Label -replace '[^A-Za-z0-9.-]', '-'))
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path $Dir 'dalvikvm.exe'
    $start.WorkingDirectory = $Dir
    $start.Arguments = (($script:vm + $Arguments | ForEach-Object { QuoteArgument $_ }) -join ' ')
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    $timedOut = $false
    try {
        if (-not $process.Start()) { throw "Could not start $Label" }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
            $process.Kill()
        }
        $process.WaitForExit()
        $outText = $stdout.Result
        $errText = $stderr.Result
        [IO.File]::WriteAllText("$base.out.log", $outText)
        [IO.File]::WriteAllText("$base.err.log", $errText)
        if ($timedOut) { throw "$Label timed out; see $base.*.log" }
        if ($process.ExitCode -ne 0) { throw "$Label exited $($process.ExitCode); see $base.*.log" }
        return $outText + "`n" + $errText
    } finally { $process.Dispose() }
}

function Passed([string] $Label) {
    $script:checks++
    Write-Host "PASS  $Label"
}

function RequireOutput([string] $Label, [string] $Text, [string[]] $Expected) {
    foreach ($marker in $Expected) {
        if (-not $Text.Contains($marker)) { throw "$Label missing expected result: $marker" }
    }
    Passed $Label
}

function SetProfile([string] $Root, [string] $Logs) {
    foreach ($sub in 'system/framework', 'system/etc/permissions', 'data', 'art', 'i18n/etc/icu') {
        New-Item -ItemType Directory -Force (Join-Path $Root $sub) | Out-Null
    }
    New-Item -ItemType Directory -Force $Logs | Out-Null
    # These fresh-profile resources are immutable for this run. Re-entering the
    # profile must not overwrite files already memory-mapped by a running ART VM.
    foreach ($resource in @(
        @($icu[0].FullName, (Join-Path $Root "i18n/etc/icu/$($icu[0].Name)")),
        @("$Dir/framework-res.apk", "$Root/system/framework/framework-res.apk"),
        @("$Dir/build.prop", "$Root/system/build.prop")
    )) {
        if (-not (Test-Path -LiteralPath $resource[1])) {
            Copy-Item -LiteralPath $resource[0] -Destination $resource[1]
        }
    }
    foreach ($library in 'org.apache.http.legacy', 'com.android.location.provider') {
        if (Test-Path -LiteralPath "$Dir/$library.jar") {
            if (-not (Test-Path -LiteralPath "$Root/system/framework/$library.jar")) {
                Copy-Item -LiteralPath "$Dir/$library.jar" -Destination "$Root/system/framework"
            }
            $declaration = "$Dir/etc/permissions/$library.xml"
            if (-not (Test-Path -LiteralPath $declaration)) {
                $declaration = Join-Path $PSScriptRoot "../../tools/package/etc/permissions/$library.xml"
            }
            if (-not (Test-Path -LiteralPath "$Root/system/etc/permissions/$library.xml")) {
                Copy-Item -LiteralPath $declaration -Destination "$Root/system/etc/permissions"
            }
        }
    }
    $env:ANDROID_ROOT = AndroidPath "$Root/system"
    $env:ANDROID_DATA = AndroidPath "$Root/data"
    $env:ANDROID_ART_ROOT = AndroidPath "$Root/art"
    $env:ANDROID_I18N_ROOT = AndroidPath "$Root/i18n"
    $env:ANDROID_TZDATA_ROOT = AndroidPath "$Dir/tzdata"
    $env:GESHER_LOGS = AndroidPath $Logs
    $env:GESHER_UID = $null
    $env:GESHER_BINDER_UID = $null
    $env:GESHER_BINDER = $null
}

function Guest([string] $Test, [string[]] $Arguments, [string] $Marker = "$Test all passed", [string] $Label = $Test) {
    $guestRoot = Join-Path $run ("guests/$Label" -replace '[^A-Za-z0-9/.-]', '-')
    SetProfile $guestRoot "$guestRoot/logs"
    $text = InvokeVm $Label (@('-cp', (AndroidPath $ServicesDex), "gesher.services.$Test") + $Arguments)
    RequireOutput $Label $text @($Marker)
}

function DeviceState {
    $state = @{}
    $file = Join-Path $script:deviceRoot 'data/system/gesher-device'
    if (Test-Path -LiteralPath $file) {
        foreach ($line in Get-Content -LiteralPath $file) {
            if ($line -match '^\s*[#!]' -or $line -notmatch '=') { continue }
            $key, $value = $line -split '=', 2
            $state[$key.Trim()] = $value.Trim()
        }
    }
    return $state
}

function RememberDevice {
    $state = DeviceState
    foreach ($name in 'system_server', 'servicemanager', 'binderd') {
        if (-not $state.ContainsKey($name)) { continue }
        $process = Get-Process -Id ([int]$state[$name]) -ErrorAction SilentlyContinue
        if (-not $process -or -not $state.ContainsKey("$name.start")) { continue }
        try {
            $started = $process.StartTime.ToFileTimeUtc()
            if ($started -eq [long]$state["$name.start"]) {
                $script:knownProcesses[$process.Id] = $started
            }
        } catch { }
    }
    if (-not $state.ContainsKey('device')) { throw 'No isolated device namespace was written.' }
    $env:GESHER_BINDER = $state['device']
    return $state
}

function UseDevice {
    SetProfile $script:deviceRoot $script:deviceLogs
    $null = RememberDevice
}

function Launcher([string] $Label, [string[]] $Arguments) {
    InvokeVm $Label (@('com.gesher.app.Launcher') + $Arguments)
}

function Tool([string] $Label, [string[]] $Arguments) {
    InvokeVm $Label (@('com.gesher.server.RuntimeCommands') + $Arguments)
}

function WaitLog([string] $Label, [string] $Path, [int] $Offset, [string[]] $Expected,
        [string] $CompletionMarker, [int] $TimeoutSeconds = 120) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $segment = ''
    do {
        $text = ReadLog $Path
        $segment = if ($text.Length -ge $Offset) { $text.Substring($Offset) } else { $text }
        if ($segment -match '(?im)(\bmicrogprobe\b[^\r\n]*\bFAIL\b|permission-cache-probe[^\r\n]*\bFAIL\b|account: failed:)') {
            throw "$Label reported failure; see $Path"
        }
        $missing = @($Expected | Where-Object { -not $segment.Contains($_) })
        if ($missing.Count -eq 0) {
            [IO.File]::WriteAllText((Join-Path $run "$Label.activity.log"), $segment)
            Passed $Label
            return $segment
        }
        if ($CompletionMarker -and $segment.Contains($CompletionMarker)) {
            throw "$Label completed without: $($missing -join ', '); see $Path"
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "$Label timed out waiting for: $($missing -join ', '); see $Path"
}

function Activity([string] $Label, [string] $Package, [string] $Class, [string[]] $Extras,
        [string[]] $Expected, [string] $CompletionMarker, [int] $TimeoutSeconds = 120) {
    $log = Join-Path $script:deviceLogs "$Package.log"
    $offset = (ReadLog $log).Length
    $null = Tool "$Label-start" (@('am', 'start', '-n', "$Package/$Class") + $Extras)
    WaitLog $Label $log $offset $Expected $CompletionMarker $TimeoutSeconds
}

try {
    Set-Location -LiteralPath $Dir
    $env:LOCALAPPDATA = Join-Path $run 'profile'
    $env:APPDATA = Join-Path $run 'profile/roaming'
    New-Item -ItemType Directory -Force $env:APPDATA | Out-Null
    $env:GESHER_MICROG_SPOOF = '1'
    $env:GESHER_SHORTCUTS = '0'
    $env:GESHER_IDLE_TIMEOUT = '3600'
    $env:GESHER_APP_WINDOW = 'headless'
    $env:GESHER_IP_GEOLOCATION = '0'
    $bcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') |
        ForEach-Object { AndroidPath "$Dir/$_.jar" }) -join ':'
    $script:vm = @("-Xbootclasspath:$bcp", "-Ximage:$(AndroidPath "$Dir/framework/boot.art")", '-Xint', '-Xusejit:false')
    $fixtures = AndroidPath $SigningFixtures
    # Separate empty device; actual Binder descriptor transfer and cancellation.
    # The CI client never commits, so no native installation dialog can appear.
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/run-package-installer.ps1" `
        -Dir $Dir -ServicesDex $ServicesDex -SigningFixtures $SigningFixtures -EvidenceDir (Join-Path $EvidenceDir 'installer')
    if ($LASTEXITCODE -ne 0) { throw 'Actual Binder package-installer regression failed.' }
    Passed 'package installer actual Binder transport and cancellation'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/run-account-chooser-native.ps1" `
        -RuntimeDir $Dir -OutputRoot (Join-Path $EvidenceDir 'chooser')
    if ($LASTEXITCODE -ne 0) { throw 'Native account chooser regression failed.' }
    Passed 'native account chooser private-desktop JNI and role integration'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/run-window-promotion.ps1" `
        -RuntimeDir $Dir -FixtureDir $Dir -OutputRoot (Join-Path $EvidenceDir 'graphics')
    if ($LASTEXITCODE -ne 0) { throw 'Actual service-first activity graphics regression failed.' }
    Passed 'service-first activity graphics and selective visibility on private desktops'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/run-ordered-broadcast.ps1" `
        -RuntimeDir $Dir -AppsDir $Dir -OutputRoot (Join-Path $EvidenceDir 'ordered')
    if ($LASTEXITCODE -ne 0) { throw 'Actual ordered broadcast goAsync/result callback regression failed.' }
    Passed 'ordered broadcast real goAsync, manifest propagation and retained final callback'
    Guest 'SigningTest' @($fixtures)
    Guest 'InstallerSigningTest' @($fixtures)
    Guest 'PackageInstallerSessionTest' @($fixtures) 'PackageInstallerSessionTest all passed:'
    $processMetadata = AndroidPath (Join-Path $SigningFixtures 'process-metadata.apk')
    Guest 'ComponentProcessTest' @($processMetadata, 'gesher.test.processmeta.SecondaryService') 'ComponentProcessTest all passed:'
    Guest 'BroadcastLifecycleTest' @($processMetadata)
    Guest 'OrderedBroadcastTest' @((AndroidPath $DevsvcApk)) 'OrderedBroadcastTest all passed:'
    Guest 'SigningRotationTest' @($fixtures, (AndroidPath (Join-Path $SigningFixtures '../identity-fixtures')))
    Guest 'IdentitySecurityTest' @($fixtures)
    $durableUids = AndroidPath (Join-Path $run 'uid-persistence')
    Guest 'IdentitySecurityTest' @('uid-write', $durableUids) 'IdentitySecurityTest all passed' 'uid-write'
    Guest 'IdentitySecurityTest' @('uid-read', $durableUids) 'existing UID registry reopened without reassignment' 'uid-read'
    Guest 'AccountSecurityTest' @()
    Guest 'AccountSecurityTest' @('authenticator-client', (AndroidPath $DevsvcApk), (AndroidPath $ProbeApk)) 'AccountSecurityTest all passed' 'authenticator-client-delegation'
    Guest 'AccountChooserTest' @((AndroidPath $DevsvcApk), (AndroidPath $ProbeApk)) 'AccountChooserTest all passed:'
    Guest 'AccountVisibilityNotificationTest' @((AndroidPath $DevsvcApk), (AndroidPath $ProbeApk)) 'AccountVisibilityNotificationTest all passed:'
    Guest 'SharedLibraryNamesTest' @() 'SharedLibraryNamesTest all passed:'
    Guest 'SdkSandboxUnsupportedTest' @() 'SdkSandboxUnsupportedTest all passed:'
    Guest 'NetworkStatsUnavailableTest' @() 'NetworkStatsUnavailableTest all passed:'
    Guest 'RollbackUnsupportedTest' @((AndroidPath (Join-Path $SigningFixtures 'unsigned.apk'))) 'RollbackUnsupportedTest all passed:'
    Guest 'WindowModeInheritanceTest' @((AndroidPath $DevsvcApk), (AndroidPath $ProbeApk)) 'WindowModeInheritanceTest all passed:'
    Guest 'ActivityWindowPromotionTest' @((AndroidPath $DevsvcApk)) 'ActivityWindowPromotionTest all passed:'
    $windowRoleUid = $env:GESHER_UID
    $windowRoleBinderUid = $env:GESHER_BINDER_UID
    try {
        $env:GESHER_UID = '12345'; $env:GESHER_BINDER_UID = '12345'
        $windowRole = InvokeVm 'window-item-client-denied' @('-cp', (AndroidPath $ServicesDex),
            'gesher.services.ActivityWindowPromotionTest', 'client-denied')
        RequireOutput 'ordinary client cannot authenticate window lifecycle metadata' $windowRole @(
            'ActivityWindowPromotionTest client role denied')
    } finally {
        $env:GESHER_UID = $windowRoleUid; $env:GESHER_BINDER_UID = $windowRoleBinderUid
    }
    Guest 'AccountSecurityTest' @('crypto-denied') 'loaded native protect/unprotect reject non-server role without plaintext fallback' 'native-crypto-denied'
    Guest 'NotificationSecurityTest' @((AndroidPath $ProbeApk), (AndroidPath $DevsvcApk))
    Guest 'SystemBindingCleanupTest' @((AndroidPath $DevsvcApk))
    Guest 'JobSecurityTest' @((AndroidPath $ProbeApk), (AndroidPath $DevsvcApk))
    Guest 'ContentSecurityTest' @((AndroidPath $DevsvcApk), (AndroidPath $ProbeApk))
    $metadata = if ($GmsCoreApk) { @(AndroidPath $GmsCoreApk) } else { @() }
    Guest 'PermissionsTest' $metadata
    foreach ($mode in '0', '1') {
        $env:GESHER_MICROG_SPOOF = $mode
        Guest 'MicrogTest' (@($fixtures) + $metadata) 'MicrogTest all passed' "microg-opt-in-$mode"
    }
    $env:GESHER_MICROG_SPOOF = '1'
    if ($GmsCoreApk) {
        Guest 'MicrogAuthenticatorTest' @((AndroidPath $GmsCoreApk), (AndroidPath $DevsvcApk))
        Guest 'PermissionRequestsTest' $metadata 'async grant/deny, result mapping and replacement race passed'
        Guest 'DispatchSecurityTest' (@($fixtures) + $metadata)
        Guest 'ProviderSecurityTest' (@($fixtures) + $metadata)
        # MicrogTest above also exercises ServiceRecoveryTest with parsed GmsCore metadata.
    } else {
        Write-Host 'SKIP  GmsCore metadata: consent-controller, service-recovery, dispatch and provider suites (supply -GmsCoreApk)'
    }

    $script:deviceRoot = Join-Path $run 'profile/Gesher'
    $script:deviceLogs = Join-Path $run 'device-first-boot'
    SetProfile $script:deviceRoot $script:deviceLogs
    if (Test-Path -LiteralPath "$Dir/fonts/fonts.xml") {
        New-Item -ItemType Directory -Force "$script:deviceRoot/system/fonts" | Out-Null
        Copy-Item -LiteralPath "$Dir/fonts/fonts.xml" -Destination "$script:deviceRoot/system/etc/fonts.xml"
        if (Test-Path -LiteralPath "$Dir/fonts/windows-fonts.xml") {
            Copy-Item -LiteralPath "$Dir/fonts/windows-fonts.xml" -Destination "$script:deviceRoot/system/etc/windows-fonts.xml"
        }
        Get-ChildItem -LiteralPath "$Dir/fonts" -Filter '*.ttf' -File |
            Copy-Item -Destination "$script:deviceRoot/system/fonts"
    }
    $null = Launcher 'install-owner' @('install', (AndroidPath $DevsvcApk))
    $firstState = RememberDevice
    $null = Launcher 'install-probe' @('install', (AndroidPath $ProbeApk))
    UseDevice
    $packageText = Tool 'installed-fixtures' @('pm', 'list', 'packages')
    # pm can expose legacy synthetic packages; check the actual installed-code registry.
    $registry = ReadLog (Join-Path $script:deviceRoot 'data/system/packages.list')
    if (-not $registry -or $registry -match 'com\.google\.android\.(gms|gsf)|com\.android\.vending') {
        throw 'Missing installed-code registry or unexpected Google APK in the fresh profile.'
    }
    RequireOutput 'only offline fixture APKs installed' $packageText @('package:com.gesher.devsvc', 'package:com.gesher.microgprobe')
    $owner = 'com.gesher.devsvc'
    $probe = 'com.gesher.microgprobe'
    $null = Activity 'real-account-authenticator' $owner '.AccountActivity' @('--ez', 'retain-account', 'true') @(
        'account: authenticator listed true', 'account: addAccount answered true',
        'account: addAccount strips unexpected credentials',
        'account: listed true, password true, user data true', 'account: set user data true',
        'account: features true false, by features 1', 'account: real token delegation and cache true',
        'account: token invalidation true', 'account: interactive authenticator continuation true',
        'account: custom token client cache and expiry true',
        'account: opaque real authenticator session and replay protection true',
        'account: real update-credentials session true',
        'account: non-token APIs strip authenticator credentials', 'account: retained account for restart verification'
    ) 'account: retained account for restart verification' 240
    $sealed = Join-Path $script:deviceRoot 'data/system/accounts.sealed'
    $record = [IO.File]::ReadAllBytes($sealed)
    $ascii = [Text.Encoding]::ASCII.GetString($record)
    if ($record.Length -le 4 -or $ascii.Substring(0, 4) -ne 'GAC2' -or
            $ascii.Contains('retained-account@example.test') -or $ascii.Contains('fake-persisted-token')) {
        throw 'Account persistence was missing, invalid or contained plaintext fixture credentials.'
    }
    Passed 'actual SystemServer sealed account storage without fixture plaintext'
    $null = Activity 'prepare-attached-probe' $owner '.AccountActivity' @('--ez', 'prepare-probe', 'true') @(
        'account: prepared hidden and visible-but-ungranted attached-probe fixtures'
    ) 'prepared hidden and visible-but-ungranted'

    # Unknown Binder PID is deliberately not an attached application, despite a claimed UID.
    $env:GESHER_UID = '12345'
    $env:GESHER_BINDER_UID = '12345'
    $text = InvokeVm 'unattached-account-denied' @('-cp', (AndroidPath $ServicesDex), 'gesher.services.AccountSecurityTest', 'remote-denied')
    RequireOutput 'unattached Binder account access denied' $text @('AccountSecurityTest all passed')
    $text = InvokeVm 'unattached-identity-denied' @('-cp', (AndroidPath $ServicesDex), 'gesher.services.IdentitySecurityTest',
        'remote-denied', $probe, 'android.permission.POST_NOTIFICATIONS')
    RequireOutput 'real Binder forged attach and permission mutation denied' $text @('IdentitySecurityTest all passed')
    $env:GESHER_UID = $null
    $env:GESHER_BINDER_UID = $null

    $probeExpected = @('PASS attached unprivileged application identity', 'PASS provider claimed package spoof',
        'PASS foreign authority publication', 'PASS secure settings mutation',
        'PASS notification denied POST does not post content',
        'PASS jobs own JobService ran and completed authenticated work', 'all passed')
    $null = Activity 'attached-unprivileged-probe' $probe '.ProbeActivity' @() $probeExpected 'all passed'
    $null = Tool 'stop-denied-probe' @('am', 'force-stop', $probe)
    $null = Activity 'owner-state-intact' $owner '.AccountActivity' @('--ez', 'verify-probe', 'true') @(
        'account: attached probe made no authenticator request or owner secret mutation true',
        'account: owner PendingIntent capability and extras preserved true'
    ) 'account: owner PendingIntent capability and extras preserved'
    $env:GESHER_BINDER_UID = '2000'
    $text = InvokeVm 'explicit-account-grant' @('-cp', (AndroidPath $ServicesDex), 'gesher.services.AccountSecurityTest', 'grant-probe', 'true')
    RequireOutput 'explicit host credential grant' $text @('AccountSecurityTest all passed')
    $env:GESHER_BINDER_UID = $null
    $null = Activity 'attached-explicit-token-grant' $probe '.ProbeActivity' @('--ez', 'expect-token', 'true') @(
        'PASS explicit credential grant authorized an attached client', 'all passed'
    ) 'all passed'
    $null = Tool 'stop-granted-probe' @('am', 'force-stop', $probe)
    $env:GESHER_BINDER_UID = '2000'
    $text = InvokeVm 'explicit-account-revoke' @('-cp', (AndroidPath $ServicesDex), 'gesher.services.AccountSecurityTest', 'grant-probe', 'false')
    RequireOutput 'explicit host credential revoke' $text @('AccountSecurityTest all passed')
    $env:GESHER_BINDER_UID = $null
    $null = Activity 'attached-probe-after-revoke' $probe '.ProbeActivity' @() $probeExpected 'all passed'
    $null = Tool 'stop-revoked-probe' @('am', 'force-stop', $probe)

    $permissionLog = Join-Path $script:deviceLogs "$probe.log"
    $offset = (ReadLog $permissionLog).Length
    $null = Tool 'permission-cache-start' @('am', 'start', '-n', "$probe/.PermissionCacheActivity")
    $initial = WaitLog 'permission-cache-initial-denial' $permissionLog $offset @('READY_INITIAL_DENIED pid=') '' 45
    if ($initial -notmatch 'READY_INITIAL_DENIED pid=(\d+)') { throw 'Missing live permission probe PID.' }
    $permissionPid = $Matches[1]
    $null = Tool 'grant-notifications' @('pm', 'grant', $probe, 'android.permission.POST_NOTIFICATIONS')
    $null = WaitLog 'permission-cache-grant-observed' $permissionLog $offset @("GRANT_OBSERVED pid=$permissionPid") '' 30
    $null = Tool 'revoke-notifications' @('pm', 'revoke', $probe, 'android.permission.POST_NOTIFICATIONS')
    $null = WaitLog 'permission-cache-revoke-same-process' $permissionLog $offset @(
        "PASS deny-grant-deny in same process pid=$permissionPid"
    ) '' 30

    $null = RememberDevice
    $null = Launcher 'shutdown-first-system-server' @('shutdown')
    $script:deviceLogs = Join-Path $run 'device-second-boot'
    SetProfile $script:deviceRoot $script:deviceLogs
    $null = Launcher 'restart-system-server' @('start')
    $secondState = RememberDevice
    if ($firstState['system_server'] -eq $secondState['system_server'] -and
            $firstState['system_server.start'] -eq $secondState['system_server.start']) {
        throw 'The account restart test did not start a second SystemServer process.'
    }
    $null = Activity 'dpapi-roundtrip-after-server-restart' $owner '.AccountActivity' @('--ez', 'verify-retained-account', 'true') @(
        'account: protected storage survived restart true', 'account: retained account removed true'
    ) 'account: retained account removed'
    Passed 'offline microG security integration complete'
} catch {
    $script:failed = $true
    [IO.File]::WriteAllText((Join-Path $run 'failure.txt'), $_.Exception.ToString())
    Write-Host "FAIL  $($_.Exception.Message)"
} finally {
    if ($script:deviceRoot -and $script:vm) {
        try {
            UseDevice
            $null = Launcher 'cleanup-isolated-device' @('shutdown')
        } catch { Write-Host 'INFO  isolated device cleanup will check recorded process identities.' }
    }
    # No process-name/global kills. PID reuse cannot target a different user's device.
    foreach ($entry in $script:knownProcesses.GetEnumerator()) {
        $process = Get-Process -Id ([int]$entry.Key) -ErrorAction SilentlyContinue
        if ($process) {
            try {
                if ($process.StartTime.ToFileTimeUtc() -eq $entry.Value) {
                    Stop-Process -Id $process.Id -Force -ErrorAction Stop
                }
            } catch { }
        }
    }
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
    Set-Location $originalLocation
    [IO.File]::WriteAllText((Join-Path $run 'result.txt'), "passed=$script:checks`nfailed=$script:failed`n")
    Write-Host "INFO  $script:checks checks passed; evidence retained at $run"
}
if ($script:failed) { exit 1 }
