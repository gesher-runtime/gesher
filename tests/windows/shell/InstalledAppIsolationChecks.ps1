# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
# This check uses the signed UI fixture and the installed desktop's live system server.
param(
    [Parameter(Mandatory)][string] $RuntimeDirectory,
    [Parameter(Mandatory)][string] $EvidenceDirectory,
    [string] $ProfileDirectory = (Join-Path $env:LOCALAPPDATA Gesher)
)
$ErrorActionPreference = 'Stop'
$package = 'com.gesher.uiapp'
$command = Join-Path $RuntimeDirectory gesher-app.cmd
$packagePattern = [regex]::Escape($package)
$null = New-Item -ItemType Directory -Force $EvidenceDirectory

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
public static class GesherInstalledIsolation {
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr handle, int timeout);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr process, int access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool GetTokenInformation(IntPtr token, int kind, out int value, int size, out int written);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string kind, string title);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out int pid);
    [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll", EntryPoint = "SendMessageW")] public static extern IntPtr Send(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", EntryPoint = "SendMessageW", CharSet = CharSet.Unicode, ExactSpelling = true)] static extern IntPtr ReadText(IntPtr window, uint message, IntPtr capacity, StringBuilder text);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool PostMessage(IntPtr window, int message, IntPtr wParam, IntPtr lParam);
    public static string Warning(IntPtr dialog) {
        var text = new StringBuilder(1024);
        ReadText(GetDlgItem(dialog, 1901), 0x000d, new IntPtr(text.Capacity), text);
        return text.ToString();
    }
    public static IntPtr Retain(int pid) {
        IntPtr process = OpenProcess(0x101000, false, pid); // PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE
        if (process == IntPtr.Zero) throw new Win32Exception();
        return process;
    }
    public static bool Exited(IntPtr process, int timeout) {
        uint state = WaitForSingleObject(process, timeout);
        if (state == 0xffffffff) throw new Win32Exception();
        return state == 0;
    }
    public static bool IsAppContainer(IntPtr process) {
        IntPtr token;
        if (!OpenProcessToken(process, 8, out token)) throw new Win32Exception();
        try {
            int value, written;
            if (!GetTokenInformation(token, 29, out value, 4, out written)) throw new Win32Exception();
            if (written != 4) throw new InvalidOperationException("Invalid AppContainer token information.");
            return value != 0;
        } finally { CloseHandle(token); }
    }
}
'@

function Start-InstalledCommand([string] $Label, [string] $Arguments) {
    $log = [IO.Path]::GetFullPath((Join-Path $EvidenceDirectory "$Label.log"))
    $line = "/d /s /c `"`"$command`" $Arguments > `"$log`" 2>&1`""
    $start = [Diagnostics.ProcessStartInfo]::new('cmd.exe', $line)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    @{ Process = [Diagnostics.Process]::Start($start); Log = $log }
}

function Wait-Command($Run, [string] $Mode) {
    if (-not $Run.Process.WaitForExit(60000)) { throw "Timed out waiting for $($Run.Log)" }
    $text = Get-Content -Raw $Run.Log
    $expectedExit = if ($Mode -eq 'cancelled') { 1223 } else { 0 }
    if ($Run.Process.ExitCode -ne $expectedExit -or $text -notmatch "(?m)^app-isolation`t$packagePattern`t$Mode\r?$") {
        throw "Isolation command did not return $Mode. See $($Run.Log)"
    }
}

function Start-AppAndCheckToken([string] $Label, [bool] $Contained) {
    $run = Start-InstalledCommand $Label "launch $package --hidden-window"
    $until = [DateTime]::UtcNow.AddSeconds(60)
    while ([DateTime]::UtcNow -lt $until -and -not $run.Process.HasExited) {
        $text = "$(Get-Content -Raw $run.Log -ErrorAction SilentlyContinue)"
        $frame = [regex]::Match($text, 'first frame drawn \(\d+ presented\), \w+, hwnd 0x([0-9a-f]+)')
        $appProcess = [regex]::Match($text, "app process for $($packagePattern): pid (\d+)")
        if ($frame.Success -and $appProcess.Success) {
            $run.Window = [IntPtr][Convert]::ToInt64($frame.Groups[1].Value, 16)
            $owner = 0
            $null = [GesherInstalledIsolation]::GetWindowThreadProcessId($run.Window, [ref]$owner)
            if ($owner -ne [int]$appProcess.Groups[1].Value) { throw "Unexpected window owner. See $($run.Log)" }
            $run.App = [GesherInstalledIsolation]::Retain($owner)
            try {
                if ([GesherInstalledIsolation]::IsAppContainer($run.App) -ne $Contained) {
                    throw "The live app window did not have the expected Windows token. See $($run.Log)"
                }
            } catch {
                $null = [GesherInstalledIsolation]::CloseHandle($run.App)
                throw
            }
            return $run
        }
        Start-Sleep -Milliseconds 100
    }
    throw "The isolated test app did not present a frame. See $($run.Log)"
}

function Choose-Override([string] $Label, [int] $Button, [string] $Result) {
    $run = Start-InstalledCommand $Label "app-isolation $package unisolated"
    $until = [DateTime]::UtcNow.AddSeconds(40)
    $dialog = [IntPtr]::Zero
    try {
        while ([DateTime]::UtcNow -lt $until -and -not $run.Process.HasExited) {
            $dialog = [GesherInstalledIsolation]::FindWindow('#32770', 'Disable isolation for this app?')
            if ($dialog -ne [IntPtr]::Zero -and [GesherInstalledIsolation]::IsWindowVisible($dialog)) {
                $owner = 0
                $null = [GesherInstalledIsolation]::GetWindowThreadProcessId($dialog, [ref]$owner)
                if ($owner -ne $serverId) { throw 'The warning belongs to a different runtime process.' }
                $defaultId = [GesherInstalledIsolation]::Send($dialog, 0x400, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64() -band 0xffff
                $text = [GesherInstalledIsolation]::Warning($dialog)
                if ($defaultId -ne 2 -or $text -notmatch $packagePattern -or
                    -not $text.Contains('full access of your Windows account')) {
                    throw 'The warning omitted the Windows access or package, or Cancel was not the default.'
                }
                if (-not [GesherInstalledIsolation]::PostMessage($dialog, 0x0111, [IntPtr]$Button, [IntPtr]::Zero)) {
                    throw 'Could not activate the verified warning button.'
                }
                $dialog = [IntPtr]::Zero
                Wait-Command $run $Result
                return
            }
            Start-Sleep -Milliseconds 100
        }
        throw "The native isolation warning did not appear. See $($run.Log)"
    } finally {
        if ($dialog -ne [IntPtr]::Zero -and $owner -eq $serverId) {
            $null = [GesherInstalledIsolation]::PostMessage($dialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        }
    }
}

$state = Get-Content -Raw (Join-Path $ProfileDirectory 'data/system/gesher-device')
if ($state -notmatch '(?m)^protected_binder=true\r?$' -or $state -notmatch '(?m)^system_server=(\d+)\r?$') {
    throw 'The desktop device did not start on the isolated route.'
}
$serverId = [int]$Matches[1]
$initial = $null
$overridden = $null
$restored = $null
$needsRestore = $false
try {
    Wait-Command (Start-InstalledCommand initial-status "app-isolation $package") isolated
    $initial = Start-AppAndCheckToken initial-launch $true
    Choose-Override cancel-warning 2 cancelled
    Wait-Command (Start-InstalledCommand cancelled-status "app-isolation $package") isolated
    if ([GesherInstalledIsolation]::Exited($initial.App, 0)) { throw 'Cancelling the warning stopped the running app.' }
    $needsRestore = $true
    Choose-Override accept-warning 6 unisolated
    if (-not [GesherInstalledIsolation]::Exited($initial.App, 30000)) { throw 'The prior isolated process was not retired.' }
    $overridden = Start-AppAndCheckToken approved-launch $false
    Wait-Command (Start-InstalledCommand restore-policy "app-isolation $package isolated") isolated
    $needsRestore = $false
    if (-not [GesherInstalledIsolation]::Exited($overridden.App, 30000)) { throw 'The prior unisolated process was not retired.' }
    $restored = Start-AppAndCheckToken restored-launch $true
    'PASS  installer: isolated app token, native warning cancel and accept, explicit override, and restored AppContainer token'
} finally {
    foreach ($run in @($initial, $overridden, $restored)) {
        if ($run -and -not $run.Process.HasExited) {
            if (-not [GesherInstalledIsolation]::Exited($run.App, 0)) {
                $null = [GesherInstalledIsolation]::PostMessage($run.Window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            }
            if (-not $run.Process.WaitForExit(30000)) { $run.Process.Kill() }
        }
        if ($run) { $null = [GesherInstalledIsolation]::CloseHandle($run.App); $run.Process.Dispose() }
    }
    if ($needsRestore) {
        Wait-Command (Start-InstalledCommand cleanup-policy "app-isolation $package isolated") isolated
    }
}
