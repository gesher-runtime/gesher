# Actual JNI/Win32 chooser contracts on owned private desktops, never SwitchDesktop.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $RuntimeDir,
    [Parameter(Mandatory)][string] $OutputRoot,
    [string] $NativeExe,
    [string] $HelperPath,
    [string] $TestExe,
    [string] $TestDex
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
function AndroidPath([string] $path) { (($path -replace '\\','/') -replace '^[A-Za-z]:','') }
function Existing([string] $path) { (Resolve-Path -LiteralPath $path -ErrorAction Stop).Path }
function QuoteArgument([string] $value) {
    $escaped = [regex]::Replace($value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    '"' + $escaped + '"'
}
$RuntimeDir = Existing $RuntimeDir
if (-not $NativeExe) { $NativeExe = Join-Path $RuntimeDir 'dalvikvm.exe' }
if (-not $HelperPath) { $HelperPath = Join-Path $RuntimeDir 'libgesher_account_crypto.so' }
if (-not $TestExe) { $TestExe = Join-Path $RuntimeDir 'account_chooser_test.exe' }
if (-not $TestDex) { $TestDex = Join-Path $RuntimeDir 'account-chooser-native-tests.dex' }
$NativeExe = Existing $NativeExe
$HelperPath = Existing $HelperPath
$TestExe = Existing $TestExe
$TestDex = Existing $TestDex
$icu = @(Get-ChildItem -LiteralPath $RuntimeDir -Filter 'icudt*l.dat' -File)
if ($icu.Count -ne 1) { throw 'Exactly one ICU data file is required.' }
$bootJars = @('core-oj.jar','core-libart.jar','core-icu4j.jar','framework.jar')
foreach ($name in ($bootJars + @('framework-res.apk','build.prop'))) {
    $null = Existing (Join-Path $RuntimeDir $name)
}
$bcp = ($bootJars | ForEach-Object { AndroidPath (Join-Path $RuntimeDir $_) }) -join ':'
$run = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('native-chooser-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
$results = @()
foreach ($mode in 'inputs','client','select','cancel','add','noadd','timer-cancel','timer-exception') {
    $profile = Join-Path $run $mode
    foreach ($part in 'system/framework','data','art','i18n/etc/icu') {
        New-Item -ItemType Directory -Path (Join-Path $profile $part) -Force | Out-Null
    }
    Copy-Item -LiteralPath (Join-Path $RuntimeDir 'framework-res.apk') -Destination (Join-Path $profile 'system/framework')
    Copy-Item -LiteralPath (Join-Path $RuntimeDir 'build.prop') -Destination (Join-Path $profile 'system')
    Copy-Item -LiteralPath $icu[0].FullName -Destination (Join-Path $profile 'i18n/etc/icu')
    $owner = if ($mode -eq 'client') { 'com.gesher.server.ChooserClient' } else { 'com.gesher.server.SystemServer' }
    $vmArgs = @($NativeExe, "-Xbootclasspath:$bcp", "-Ximage:$(AndroidPath (Join-Path $RuntimeDir 'framework/boot.art'))",
        '-Xint','-Xusejit:false','-cp',(AndroidPath $TestDex),$owner,(AndroidPath $HelperPath),$mode)
    if ($mode -eq 'client') { $vmArgs += 'com.gesher.server.SystemServer' }
    $commandFile = Join-Path $profile 'command.txt'
    [IO.File]::WriteAllText($commandFile, (($vmArgs | ForEach-Object { QuoteArgument $_ }) -join ' '), [Text.Encoding]::Unicode)
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $TestExe
    $start.Arguments = (@($commandFile,$mode,$RuntimeDir) | ForEach-Object { QuoteArgument $_ }) -join ' '
    $start.WorkingDirectory = $RuntimeDir
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables['ANDROID_ROOT'] = AndroidPath (Join-Path $profile 'system')
    $start.EnvironmentVariables['ANDROID_DATA'] = AndroidPath (Join-Path $profile 'data')
    $start.EnvironmentVariables['ANDROID_ART_ROOT'] = AndroidPath (Join-Path $profile 'art')
    $start.EnvironmentVariables['ANDROID_I18N_ROOT'] = AndroidPath (Join-Path $profile 'i18n')
    $start.EnvironmentVariables['ANDROID_TZDATA_ROOT'] = AndroidPath (Join-Path $RuntimeDir 'tzdata')
    $start.EnvironmentVariables['LOCALAPPDATA'] = $profile
    $start.EnvironmentVariables['APPDATA'] = Join-Path $profile 'roaming'
    foreach ($name in 'GESHER_BINDER','GESHER_BINDER_UID','GESHER_UID','GESHER_REQUIRE_BOOT_IMAGE') {
        $start.EnvironmentVariables.Remove($name)
    }
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    $null = $process.Start()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $finished = $process.WaitForExit(45000)
    if (-not $finished) { $process.Kill(); $null = $process.WaitForExit(5000) }
    $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
    $log = Join-Path $profile 'fixture.log'
    [IO.File]::WriteAllText($log, $output, [Text.Encoding]::UTF8)
    $passed = $finished -and $process.ExitCode -eq 0 -and $output.Contains('account_chooser_test all passed:') `
        -and $output.Contains('AccountChooserNative') -and $output.Contains('all passed')
    $results += [pscustomobject]@{Mode=$mode;Passed=$passed;ExitCode=$process.ExitCode;Log=$log}
    $process.Dispose()
    if (-not $passed) { Write-Host $output; throw "Native chooser fixture failed: $mode" }
    Write-Host "PASS  native account chooser: $mode"
}
$receipt = [pscustomobject]@{
    Scope='Synthetic JNI/Win32 dialogs on private never-switched desktops; no accounts or app jar loaded'
    NativeSha256=(Get-FileHash -LiteralPath $NativeExe -Algorithm SHA256).Hash.ToLowerInvariant()
    HelperSha256=(Get-FileHash -LiteralPath $HelperPath -Algorithm SHA256).Hash.ToLowerInvariant()
    ControllerSha256=(Get-FileHash -LiteralPath $TestExe -Algorithm SHA256).Hash.ToLowerInvariant()
    TestDexSha256=(Get-FileHash -LiteralPath $TestDex -Algorithm SHA256).Hash.ToLowerInvariant()
    Results=$results
}
$receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $run 'validation.json') -Encoding UTF8
Write-Host 'AccountChooserNative all 8 modes passed'
