# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# gesher-app.exe (compat/shell/gesher_app.cc): what gesher-launch.exe and
# gesher-app.cmd run in production in place of gesher-app.ps1 (no PowerShell
# startup, no ExecutionPolicy). A copy of the built program runs in a temporary
# installation next to a stand-in dalvikvm.exe that records its arguments and
# environment, so this runs without a runtime. It checks the script's contract:
# every gesher-app.ps1 command reaches com.gesher.app.Launcher, the runtime
# files and their version stamp reach the profile, ANDROID_* and the profile
# variables, GESHER_LAUNCH_ERROR_FILE moved to --launch-error-file, exit codes,
# --single-process, cameras, portable layout, and that a VM which cannot start
# fails. With -LauncherPath, gesher-launch.exe runs that gesher-app.exe.
param(
    [Parameter(Mandatory = $true)][string] $AppPath,
    [string] $LauncherPath = '',
    [string] $ScriptPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-app.ps1')
)

$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'gesher-app.exe checks require Windows.' }
$AppPath = (Get-Item -LiteralPath $AppPath).FullName
$script:checks = 0
function Assert-NativeApp([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    $script:checks++
}

$work = Join-Path ([IO.Path]::GetTempPath()) ('gesher-native-app-' + [Guid]::NewGuid().ToString('N'))
$install = Join-Path $work 'install'
$profileDir = Join-Path $work 'profile'
New-Item -ItemType Directory -Force $install, $profileDir | Out-Null

# The stand-in VM: one line per argument, then the environment it got.
$standInSource = @'
using System;
using System.IO;
using System.Text;
public static class StandInVm {
    public static int Main(string[] args) {
        var probe = Environment.GetEnvironmentVariable("GESHER_TEST_PROBE");
        var sb = new StringBuilder();
        foreach (var a in args) sb.Append("arg=").Append(a).Append('\n');
        foreach (var name in new[] { "ANDROID_ROOT", "ANDROID_ART_ROOT", "ANDROID_DATA", "ANDROID_I18N_ROOT",
                                     "ANDROID_TZDATA_ROOT", "GESHER_PROFILE", "GESHER_PORTABLE",
                                     "GESHER_LAUNCH_ERROR_FILE", "GESHER_SANDBOX", "GESHER_MICROG_SPOOF",
                                     "GESHER_CAMERA_REAR_ID" }) {
            sb.Append("env:").Append(name).Append('=').Append(Environment.GetEnvironmentVariable(name) ?? "<unset>").Append('\n');
        }
        sb.Append("cwd=").Append(Environment.CurrentDirectory).Append('\n');
        if (!string.IsNullOrEmpty(probe)) File.WriteAllText(probe, sb.ToString(), new UTF8Encoding(false));
        Console.WriteLine("stand-in vm output");
        int code;
        return int.TryParse(Environment.GetEnvironmentVariable("GESHER_TEST_EXIT"), out code) ? code : 0;
    }
}
'@
$standIn = Join-Path $work 'dalvikvm.exe'
Add-Type -TypeDefinition $standInSource -OutputAssembly $standIn -OutputType ConsoleApplication

function New-Installation([string] $Directory) {
    New-Item -ItemType Directory -Force $Directory | Out-Null
    Copy-Item -LiteralPath $AppPath (Join-Path $Directory 'gesher-app.exe') -Force
    Copy-Item -LiteralPath $standIn (Join-Path $Directory 'dalvikvm.exe') -Force
    [IO.File]::WriteAllText((Join-Path $Directory 'icudt76l.dat'), 'icu')
    [IO.File]::WriteAllText((Join-Path $Directory 'framework-res.apk'), 'framework-res')
    [IO.File]::WriteAllText((Join-Path $Directory 'build.prop'), "ro.build.version.sdk=36`n")
    [IO.File]::WriteAllText((Join-Path $Directory 'gesher-version.ini'), "[Gesher]`r`nVersion=1.2.3`r`n")
}
New-Installation $install

$saved = @{}
foreach ($name in 'GESHER_PROFILE', 'GESHER_PORTABLE', 'GESHER_LAUNCH_ERROR_FILE', 'GESHER_TEST_PROBE',
                  'GESHER_TEST_EXIT', 'GESHER_SANDBOX', 'GESHER_MICROG_SPOOF', 'GESHER_CAMERA_REAR_ID') {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name)
}
$probe = Join-Path $work 'probe.txt'

function Invoke-NativeApp([string] $Exe, [string[]] $Arguments, [int] $Exit = 0) {
    Remove-Item -LiteralPath $probe -ErrorAction SilentlyContinue
    $env:GESHER_TEST_PROBE = $probe
    $env:GESHER_TEST_EXIT = "$Exit"
    $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
    $psi.Arguments = (($Arguments | ForEach-Object { if ($_ -match '[\s"]' -or $_ -eq '') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' ')
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = $work
    $p = [Diagnostics.Process]::Start($psi)
    $err = $p.StandardError.ReadToEndAsync()
    $out = $p.StandardOutput.ReadToEndAsync()
    if (-not $p.WaitForExit(60000)) { $p.Kill(); throw "$Exe $($psi.Arguments) did not exit within 60 s" }
    $p.WaitForExit()
    $lines = if (Test-Path -LiteralPath $probe) { @([IO.File]::ReadAllLines($probe)) } else { @() }
    [pscustomobject]@{
        Code = $p.ExitCode
        Output = $out.Result + $err.Result
        Args = @($lines | Where-Object { $_.StartsWith('arg=') } | ForEach-Object { $_.Substring(4) })
        Env = $lines | Where-Object { $_.StartsWith('env:') } | ForEach-Object -Begin { $h = @{} } -Process {
            $k, $v = $_.Substring(4) -split '=', 2; $h[$k] = $v } -End { $h }
    }
}

function Get-MainClassArguments($Result) {
    $index = [Array]::IndexOf([string[]] $Result.Args, ($Result.Args | Where-Object { $_ -like 'com.gesher.*' } | Select-Object -First 1))
    if ($index -lt 0) { return @() }
    return @($Result.Args | Select-Object -Skip $index)
}

try {
    $env:GESHER_PROFILE = $profileDir
    Remove-Item Env:GESHER_PORTABLE, Env:GESHER_LAUNCH_ERROR_FILE, Env:GESHER_MICROG_SPOOF, Env:GESHER_CAMERA_REAR_ID -ErrorAction SilentlyContinue
    $env:GESHER_SANDBOX = '1'
    $exe = Join-Path $install 'gesher-app.exe'
    $drive = $install.Substring(0, 2)
    $noDrive = { param([string] $p) $p.Substring(2).Replace('\', '/') }

    # Every command gesher-app.ps1 hands to com.gesher.app.Launcher reaches it from the exe.
    $ast = [Management.Automation.Language.Parser]::ParseFile((Resolve-Path $ScriptPath), [ref]$null, [ref]$null)
    $assignment = $ast.Find({ param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and
        $n.Left.Extent.Text -eq '$commands' }, $true)
    Assert-NativeApp ($null -ne $assignment) 'gesher-app.ps1 assigns $commands'
    $commands = @($assignment.Right.FindAll({ param($n) $n -is [Management.Automation.Language.StringConstantExpressionAst] }, $true) | ForEach-Object { $_.Value })
    Assert-NativeApp ($commands.Count -ge 20) "gesher-app.ps1's command list was read ($($commands.Count))"
    $apk = Join-Path $work 'app one.apk'
    [IO.File]::WriteAllText($apk, 'apk')
    foreach ($command in $commands) {
        if ($command -in 'shutdown', 'cameras', 'apk-info') { continue }
        $arguments = if ($command -in 'install', 'apk-icon') { @($command, $apk) } else { @($command, 'com.example', 'x y') }
        $r = Invoke-NativeApp $exe $arguments
        $main = Get-MainClassArguments $r
        Assert-NativeApp ($r.Code -eq 0) "gesher-app.exe $command exits 0: $($r.Code) $($r.Output)"
        Assert-NativeApp ($main.Count -ge 2 -and $main[0] -eq 'com.gesher.app.Launcher' -and $main[1] -eq $command) "gesher-app.exe $command runs com.gesher.app.Launcher $command (got: $($main -join ' | '))"
        if ($command -in 'install', 'apk-icon') {
            Assert-NativeApp ($main[2] -eq (& $noDrive $apk).Replace('\', '/')) "$command passes the APK's long path without its drive: $($main[2])"
        } else {
            Assert-NativeApp ((@($main | Select-Object -Skip 2) -join '|') -eq 'com.example|x y') "$command passes its arguments unchanged: $($main -join ' | ')"
        }
    }

    # The VM's options, environment and the profile's runtime files.
    $r = Invoke-NativeApp $exe @('list')
    $bcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { & $noDrive (Join-Path $install "$_.jar") }) -join ':'
    Assert-NativeApp ($r.Args[0] -eq "-Xbootclasspath:$bcp") "boot class path: $($r.Args[0])"
    Assert-NativeApp ($r.Args[1] -eq "-Ximage:$(& $noDrive (Join-Path $install 'framework\boot.art'))") "boot image: $($r.Args[1])"
    Assert-NativeApp ($r.Args[2] -eq '-Xusejit:false') "JIT option: $($r.Args[2])"
    foreach ($pair in @(@('ANDROID_ROOT', 'system'), @('ANDROID_ART_ROOT', 'art'), @('ANDROID_DATA', 'data'), @('ANDROID_I18N_ROOT', 'i18n'))) {
        $expected = & $noDrive (Join-Path $profileDir $pair[1])
        Assert-NativeApp ($r.Env[$pair[0]] -eq $expected) "$($pair[0]) is $expected without a drive: $($r.Env[$pair[0]])"
    }
    Assert-NativeApp ($r.Env['ANDROID_TZDATA_ROOT'] -eq (& $noDrive (Join-Path $install 'tzdata'))) "ANDROID_TZDATA_ROOT: $($r.Env['ANDROID_TZDATA_ROOT'])"
    Assert-NativeApp ($r.Env['GESHER_PROFILE'] -eq $profileDir) "GESHER_PROFILE: $($r.Env['GESHER_PROFILE'])"
    Assert-NativeApp ($r.Env['GESHER_PORTABLE'] -eq '0') "GESHER_PORTABLE in an ordinary installation: $($r.Env['GESHER_PORTABLE'])"
    Assert-NativeApp ($r.Env['GESHER_SANDBOX'] -eq '<unset>') "GESHER_SANDBOX is removed: $($r.Env['GESHER_SANDBOX'])"
    Assert-NativeApp ($r.Env['GESHER_LAUNCH_ERROR_FILE'] -eq '<unset>') 'no launch error file when none was given'
    foreach ($file in 'system\framework\framework-res.apk', 'system\build.prop', 'i18n\etc\icu\icudt76l.dat') {
        Assert-NativeApp (Test-Path -LiteralPath (Join-Path $profileDir $file) -PathType Leaf) "the runtime file $file reached the profile"
    }
    $stamp = [IO.File]::ReadAllText((Join-Path $profileDir 'system\.gesher-runtime-app.version'))
    Assert-NativeApp ($stamp -eq ([IO.Path]::GetFullPath($install) + "`n1.2.3")) "the runtime version stamp: $stamp"
    # A changed runtime file with the same size and stamp is refreshed after an upgrade.
    [IO.File]::WriteAllText((Join-Path $install 'build.prop'), "ro.build.version.sdk=37`n")
    (Get-Item (Join-Path $install 'build.prop')).LastWriteTimeUtc = (Get-Item (Join-Path $profileDir 'system\build.prop')).LastWriteTimeUtc
    [IO.File]::WriteAllText((Join-Path $install 'gesher-version.ini'), "[Gesher]`r`nVersion=1.2.4`r`n")
    $r = Invoke-NativeApp $exe @('list')
    Assert-NativeApp ([IO.File]::ReadAllText((Join-Path $profileDir 'system\build.prop')) -eq "ro.build.version.sdk=37`n") 'an upgrade refreshes an equal-size runtime file'

    # The caller's launch error file goes to the Launcher as an argument, not the environment.
    $env:GESHER_LAUNCH_ERROR_FILE = Join-Path $work 'launch error.txt'
    $r = Invoke-NativeApp $exe @($apk, '--detach')
    Remove-Item Env:GESHER_LAUNCH_ERROR_FILE
    $main = Get-MainClassArguments $r
    Assert-NativeApp (($main -join '|') -eq "com.gesher.app.Launcher|--launch-error-file|$(Join-Path $work 'launch error.txt')|launch|$((& $noDrive $apk))|--detach") "an APK launch with a launch error file: $($main -join ' | ')"
    Assert-NativeApp ($r.Env['GESHER_LAUNCH_ERROR_FILE'] -eq '<unset>') "the VM does not inherit GESHER_LAUNCH_ERROR_FILE: $($r.Env['GESHER_LAUNCH_ERROR_FILE'])"

    # Exit codes and output pass through.
    $r = Invoke-NativeApp $exe @('status') 7
    Assert-NativeApp ($r.Code -eq 7) "the VM's exit code passes through: $($r.Code)"
    Assert-NativeApp ($r.Output.Contains('stand-in vm output')) "the VM's output passes through: $($r.Output)"

    # --single-process: the app in this process (com.gesher.app.Main), the APK last.
    $r = Invoke-NativeApp $exe @($apk, '--Single-Process', '--headless')
    $main = Get-MainClassArguments $r
    Assert-NativeApp (($main -join '|') -eq "com.gesher.app.Main|--headless|$((& $noDrive $apk))") "--single-process: $($main -join ' | ')"

    # cameras: the selection with the profile's mounting file, and its recorded choice.
    [IO.File]::WriteAllText((Join-Path $profileDir 'camera-rear'), ([char]0xFEFF) + "cam-1`r`n", [Text.UTF8Encoding]::new($true))
    $r = Invoke-NativeApp $exe @('cameras')
    $main = Get-MainClassArguments $r
    Assert-NativeApp (($main -join '|') -eq "com.gesher.server.CameraSelection|$(& $noDrive (Join-Path $profileDir 'camera-rear'))") "cameras: $($main -join ' | ')"
    Assert-NativeApp ($r.Env['GESHER_CAMERA_REAR_ID'] -eq 'cam-1') "the recorded rear camera: $($r.Env['GESHER_CAMERA_REAR_ID'])"
    Remove-Item -LiteralPath (Join-Path $profileDir 'camera-rear')

    # apk-info: APK, output file and snapshot directory.
    $snapshot = Join-Path $work 'snapshot'
    New-Item -ItemType Directory -Force $snapshot | Out-Null
    $r = Invoke-NativeApp $exe @('apk-info', $apk, (Join-Path $work 'info.txt'), $snapshot)
    $main = Get-MainClassArguments $r
    Assert-NativeApp (($main -join '|') -eq "com.gesher.app.Launcher|apk-info|$(& $noDrive $apk)|$(& $noDrive (Join-Path $work 'info.txt'))|$(& $noDrive $snapshot)") "apk-info: $($main -join ' | ')"
    $r = Invoke-NativeApp $exe @('apk-info', $apk)
    Assert-NativeApp ($r.Code -eq 2) "apk-info without OUT is a usage error: $($r.Code)"

    # --system-server: a device of this run (stand-in binderd and servicemanager), then the
    # app in com.gesher.app.Main; the run's logs; a device program that cannot start fails.
    foreach ($program in 'binderd.exe', 'servicemanager.exe') { Copy-Item -LiteralPath $standIn (Join-Path $install $program) -Force }
    $r = Invoke-NativeApp $exe @($apk, '--system-server')
    $main = Get-MainClassArguments $r
    Assert-NativeApp ($r.Code -eq 0) "--system-server exits with the app's code: $($r.Code) $($r.Output)"
    Assert-NativeApp (($main -join '|') -eq "com.gesher.app.Main|--system-server|$((& $noDrive $apk))") "--system-server runs the app last: $($main -join ' | ')"
    foreach ($log in 'run-binderd.log', 'run-servicemanager.log', 'run-system_server.log') {
        Assert-NativeApp (Test-Path -LiteralPath (Join-Path $profileDir "logs\$log") -PathType Leaf) "--system-server writes logs\$log"
    }
    Remove-Item -LiteralPath (Join-Path $install 'servicemanager.exe')
    $r = Invoke-NativeApp $exe @($apk, '--system-server')
    Assert-NativeApp ($r.Code -eq 1 -and $r.Output.Contains('could not start') -and $r.Output.Contains('servicemanager.exe')) "a missing servicemanager fails the run: $($r.Code) $($r.Output)"
    Remove-Item -LiteralPath (Join-Path $install 'binderd.exe')

    # No arguments: usage, exit 2. Shutdown with no device: nothing to stop.
    $r = Invoke-NativeApp $exe @()
    Assert-NativeApp ($r.Code -eq 2 -and $r.Output.Contains('accounts')) "usage: $($r.Code) $($r.Output)"
    $r = Invoke-NativeApp $exe @('shutdown')
    Assert-NativeApp ($r.Code -eq 0 -and $r.Args.Count -eq 0) "shutdown without a device: $($r.Code) $($r.Output)"

    # Portable layout: App\ next to portable.marker, the profile in Data.
    Remove-Item Env:GESHER_PROFILE
    $portable = Join-Path $work 'portable'
    New-Installation (Join-Path $portable 'App')
    [IO.File]::WriteAllText((Join-Path $portable 'portable.marker'), '')
    $r = Invoke-NativeApp (Join-Path $portable 'App\gesher-app.exe') @('list')
    Assert-NativeApp ($r.Env['GESHER_PORTABLE'] -eq '1') "GESHER_PORTABLE in a portable layout: $($r.Env['GESHER_PORTABLE'])"
    Assert-NativeApp ($r.Env['GESHER_PROFILE'] -eq (Join-Path $portable 'Data')) "the portable profile: $($r.Env['GESHER_PROFILE'])"
    $env:GESHER_PROFILE = $profileDir

    # A VM that cannot start fails, with a message.
    Remove-Item -LiteralPath (Join-Path $install 'dalvikvm.exe')
    $r = Invoke-NativeApp $exe @('list')
    Assert-NativeApp ($r.Code -ne 0 -and $r.Output.Contains('could not start')) "a missing VM fails: $($r.Code) $($r.Output)"
    Copy-Item -LiteralPath $standIn (Join-Path $install 'dalvikvm.exe') -Force

    # gesher-launch.exe runs gesher-app.exe (not PowerShell) when it is installed.
    if ($LauncherPath) {
        Copy-Item -LiteralPath (Get-Item -LiteralPath $LauncherPath).FullName (Join-Path $install 'gesher-launch.exe') -Force
        $r = Invoke-NativeApp (Join-Path $install 'gesher-launch.exe') @('launch', 'com.example two', 'tail') 5
        $main = Get-MainClassArguments $r
        Assert-NativeApp ($r.Code -eq 5) "gesher-launch passes on gesher-app.exe's exit code: $($r.Code) $($r.Output)"
        Assert-NativeApp (($main -join '|') -eq 'com.gesher.app.Launcher|launch|com.example two|tail') "gesher-launch -> gesher-app.exe -> Launcher: $($main -join ' | ')"
        Assert-NativeApp ($r.Output.Contains('stand-in vm output')) "gesher-launch passes on the output: $($r.Output)"
    }
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "PASS  gesher-app.exe: $script:checks native launcher checks"
