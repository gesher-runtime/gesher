# Focused ordinary APK removal: real durable paths, local consent injection, no real user data or dialogs.
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $RuntimeDir,
    [Parameter(Mandatory)] [string] $TargetV1,
    [Parameter(Mandatory)] [string] $TargetV2,
    [Parameter(Mandatory)] [string] $CallerApk,
    [string] $ServicesDex,
    [Parameter(Mandatory)] [string] $OutputRoot
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$RuntimeDir = (Resolve-Path -LiteralPath $RuntimeDir).Path
$TargetV1 = (Resolve-Path -LiteralPath $TargetV1).Path
$TargetV2 = (Resolve-Path -LiteralPath $TargetV2).Path
$CallerApk = (Resolve-Path -LiteralPath $CallerApk).Path
if (-not $ServicesDex) { $ServicesDex = Join-Path $RuntimeDir 'services-tests.dex' }
$ServicesDex = (Resolve-Path -LiteralPath $ServicesDex).Path
foreach ($relative in 'dalvikvm.exe','core-oj.jar','core-libart.jar','core-icu4j.jar',
        'framework.jar','gesher-app.jar','framework-res.apk','build.prop') {
    if (-not (Test-Path -LiteralPath (Join-Path $RuntimeDir $relative) -PathType Leaf)) {
        throw "Missing focused contract input: $relative"
    }
}
$images = @(Get-ChildItem -LiteralPath (Join-Path $RuntimeDir 'framework/x86_64') -File |
    Where-Object { $_.Name -match '^boot(?:-core-libart|-core-icu4j|-framework|-gesher-app)?\.(art|oat|vdex)$' })
if ($images.Count -ne 15) { throw 'A complete matching five-image boot chain is mandatory.' }
$work = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('package-uninstall-' + [guid]::NewGuid().ToString('N'))
$profile = Join-Path $work 'profile'
foreach ($sub in 'system/framework','data','art','i18n/etc/icu') {
    New-Item -ItemType Directory -Path (Join-Path $profile $sub) -Force | Out-Null
}
Copy-Item -LiteralPath (Join-Path $RuntimeDir 'framework-res.apk') -Destination (Join-Path $profile 'system/framework')
Copy-Item -LiteralPath (Join-Path $RuntimeDir 'build.prop') -Destination (Join-Path $profile 'system')
$icu = @(Get-ChildItem -LiteralPath $RuntimeDir -Filter 'icudt*l.dat' -File)
if ($icu.Count -ne 1) { throw 'Exactly one ICU input is required.' }
Copy-Item -LiteralPath $icu[0].FullName -Destination (Join-Path $profile 'i18n/etc/icu')
function AndroidPath([string] $Path) {
    $value = [IO.Path]::GetFullPath($Path) -replace '\\','/'
    if ($value -notmatch '^([A-Za-z]):(.*)$') { throw 'Unsupported runtime path.' }
    if ($Matches[1] -ieq $RuntimeDir.Substring(0,1)) { return $Matches[2] }
    return "/mnt/$($Matches[1].ToLowerInvariant())$($Matches[2])"
}
function QuoteArgument([string] $Value) {
    if ($Value -eq '') { return '""' }
    if ($Value -notmatch '[\s"]') { return $Value }
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}
$names = @('LOCALAPPDATA','APPDATA','ANDROID_ROOT','ANDROID_DATA','ANDROID_ART_ROOT',
    'ANDROID_I18N_ROOT','ANDROID_TZDATA_ROOT','GESHER_BINDER','GESHER_BINDER_UID','GESHER_UID',
    'GESHER_REQUIRE_BOOT_IMAGE','GESHER_MICROG_SPOOF','GESHER_IP_GEOLOCATION','GESHER_APP_WINDOW')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$process = $null
$started = $false
try {
    $env:LOCALAPPDATA = Join-Path $profile 'local'
    $env:APPDATA = Join-Path $profile 'roaming'
    $env:ANDROID_ROOT = AndroidPath (Join-Path $profile 'system')
    $env:ANDROID_DATA = AndroidPath (Join-Path $profile 'data')
    $env:ANDROID_ART_ROOT = AndroidPath (Join-Path $profile 'art')
    $env:ANDROID_I18N_ROOT = AndroidPath (Join-Path $profile 'i18n')
    $env:ANDROID_TZDATA_ROOT = AndroidPath (Join-Path $RuntimeDir 'tzdata')
    $env:GESHER_BINDER = $null
    $env:GESHER_BINDER_UID = '1000'; $env:GESHER_UID = '1000'
    $env:GESHER_REQUIRE_BOOT_IMAGE = '1'; $env:GESHER_MICROG_SPOOF = '1'
    $env:GESHER_IP_GEOLOCATION = '0'; $env:GESHER_APP_WINDOW = 'headless'
    $bcp = (@('core-oj','core-libart','core-icu4j','framework','gesher-app') |
        ForEach-Object { AndroidPath (Join-Path $RuntimeDir ($_ + '.jar')) }) -join ':'
    $arguments = @("-Djava.io.tmpdir=$(AndroidPath (Join-Path $profile 'data'))", "-Xbootclasspath:$bcp",
        "-Ximage:$(AndroidPath (Join-Path $RuntimeDir 'framework/boot.art'))", '-Xint','-Xusejit:false',
        '-cp',(AndroidPath $ServicesDex),'gesher.services.ServiceFixtureRunner','PackageUninstallTest',
        (AndroidPath $TargetV1),(AndroidPath $TargetV2),(AndroidPath $CallerApk))
    $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $RuntimeDir 'dalvikvm.exe'))
    $start.Arguments = ($arguments | ForEach-Object { QuoteArgument $_ }) -join ' '
    $start.WorkingDirectory = $RuntimeDir
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $start
    if (-not $process.Start()) { throw 'Could not start focused contract fixture.' }
    $started = $true
    $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(60000)) { $process.Kill(); throw "Contract fixture timed out: $work" }
    $process.WaitForExit()
    [IO.File]::WriteAllText((Join-Path $work 'fixture.out.log'), $stdout.Result, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $work 'fixture.err.log'), $stderr.Result, [Text.UTF8Encoding]::new($false))
    $combined = $stdout.Result + "`n" + $stderr.Result
    if ($process.ExitCode -ne 0 -or $combined -notmatch 'PackageUninstallTest all passed: (\d+) checks') {
        throw "Package uninstall contract failed exit=$($process.ExitCode): $work"
    }
    $checks = [int]$Matches[1]
    $marker = 'gesher: boot image loaded: 5 spaces (boot.art boot-core-libart.art boot-core-icu4j.art boot-framework.art boot-gesher-app.art)'
    $loads = @($combined -split "`n" | Where-Object { $_.Contains('gesher: boot image loaded:') })
    if ($loads.Count -ne 1 -or -not $loads[0].Contains($marker)) { throw 'Complete five-space runtime boot was not verified.' }
    $hashes = @()
    foreach ($path in @((Join-Path $RuntimeDir 'dalvikvm.exe'),(Join-Path $RuntimeDir 'framework.jar'),
            (Join-Path $RuntimeDir 'gesher-app.jar'),$ServicesDex,$TargetV1,$TargetV2,$CallerApk) + @($images.FullName)) {
        $hashes += @{path=$path;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
    $receipt = @{scope='Synthetic ordinary removal: local explicit-consent injection, real storage/capability paths, denial/native-unavailable/recovery/reinstall regressions; no actual user dialogs/data';
        passed=$true;checks=$checks;completeBootSpaces=5;inputs=$hashes;
        stdoutSha256=(Get-FileHash -LiteralPath (Join-Path $work 'fixture.out.log') -Algorithm SHA256).Hash.ToLowerInvariant();
        stderrSha256=(Get-FileHash -LiteralPath (Join-Path $work 'fixture.err.log') -Algorithm SHA256).Hash.ToLowerInvariant()}
    [IO.File]::WriteAllText((Join-Path $work 'validation.json'),($receipt|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
    Write-Output "PackageUninstallTest all passed: $checks checks"
    Write-Output "Evidence: $work"
} finally {
    if ($process) { if ($started -and -not $process.HasExited) { $process.Kill() }; $process.Dispose() }
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
