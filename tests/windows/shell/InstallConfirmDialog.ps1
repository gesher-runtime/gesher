# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# UI automation for the dialogs gesher-launch shows when an APK or split bundle
# is opened from Explorer (compat/shell/gesher_launch.cc, docs/installer.md,
# "Opening an APK from Explorer", issue 519). Dot-source it; Windows PowerShell
# 5.1 and pwsh 7 both work. Nothing here starts or installs anything: the caller
# starts gesher-launch (the registered association command, or portable
# Gesher.exe with a file) and this answers the dialogs the way a user would.
#
# The dialogs are top-level dialogs of the gesher-launch process, found by their
# stable control IDs (documented in gesher_launch.cc and docs/installer.md):
#   confirmation: title install_confirm_title (tools/package/ui-messages.json),
#                 Install/Update = IDOK (1), Cancel = IDCANCEL (2),
#                 "Create a desktop shortcut" check box = 1001 (absent or hidden
#                 in portable mode, with GESHER_SHORTCUTS=0 and when the
#                 package's recorded desktop shortcut exists)
#   installed:    title install_done_title, Open = 1002, Done = IDCANCEL (2)
# Buttons are pressed with BM_CLICK, posted to the button (a sent BM_CLICK would
# wait for everything the click starts, such as the whole install); if the
# dialog ignores it (BM_CLICK can be dropped by an inactive dialog) the click's
# own notification, WM_COMMAND BN_CLICKED, is posted to the dialog instead.

$script:GesherInstallDialogIdOk = 1
$script:GesherInstallDialogIdCancel = 2
$script:GesherInstallDialogIdDesktop = 1001
$script:GesherInstallDialogIdOpen = 1002

if (-not ('GesherInstallDialogNative' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class GesherInstallDialogNative {
  delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc proc, IntPtr lparam);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dialog, int id);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr hwnd, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr hwnd, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern IntPtr SendMessageTimeoutW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l,
      uint flags, uint timeout, out IntPtr result);
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct ProcessEntry {
    public uint size, usage, pid; public IntPtr heap; public uint module, threads, parent; public int priority; public uint flags;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string exe; }
  [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool Process32FirstW(IntPtr snapshot, ref ProcessEntry entry);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool Process32NextW(IntPtr snapshot, ref ProcessEntry entry);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);

  // `root` and every process descended from it (one Toolhelp snapshot).
  public static uint[] Tree(uint root) {
    var children = new Dictionary<uint, List<uint>>();
    IntPtr snapshot = CreateToolhelp32Snapshot(2, 0);  // TH32CS_SNAPPROCESS
    if (snapshot != new IntPtr(-1)) {
      try {
        var e = new ProcessEntry(); e.size = (uint) Marshal.SizeOf(typeof(ProcessEntry));
        for (bool ok = Process32FirstW(snapshot, ref e); ok; ok = Process32NextW(snapshot, ref e)) {
          if (e.pid == e.parent) continue;
          List<uint> list;
          if (!children.TryGetValue(e.parent, out list)) children[e.parent] = list = new List<uint>();
          list.Add(e.pid);
        }
      } finally { CloseHandle(snapshot); }
    }
    var result = new List<uint> { root };
    for (int i = 0; i < result.Count; i++) {
      List<uint> list;
      if (children.TryGetValue(result[i], out list)) foreach (uint c in list) if (!result.Contains(c)) result.Add(c);
    }
    return result.ToArray();
  }
  // The visible top-level windows of the given processes (all processes when `pids` is null).
  public static IntPtr[] Windows(uint[] pids) {
    var set = pids == null ? null : new HashSet<uint>(pids);
    var found = new List<IntPtr>();
    EnumWindows((h, l) => {
      uint pid;
      GetWindowThreadProcessId(h, out pid);
      if ((set == null || set.Contains(pid)) && IsWindowVisible(h)) found.Add(h);
      return true;
    }, IntPtr.Zero);
    return found.ToArray();
  }
  public static uint ProcessId(IntPtr hwnd) { uint pid; GetWindowThreadProcessId(hwnd, out pid); return pid; }
  public static string Title(IntPtr hwnd) { var s = new StringBuilder(512); GetWindowTextW(hwnd, s, s.Capacity); return s.ToString(); }
  public static string ClassName(IntPtr hwnd) { var s = new StringBuilder(256); GetClassNameW(hwnd, s, s.Capacity); return s.ToString(); }
  // A dialog item that exists and is shown.
  public static bool Shown(IntPtr dialog, int id) {
    IntPtr item = GetDlgItem(dialog, id);
    return item != IntPtr.Zero && IsWindowVisible(item);
  }
  // BM_GETCHECK of a dialog item: 0, 1, 2, or -1 (no item, or no answer within 5 s).
  public static int Check(IntPtr dialog, int id) {
    IntPtr item = GetDlgItem(dialog, id), result;
    if (item == IntPtr.Zero) return -1;
    if (SendMessageTimeoutW(item, 0x00F0, IntPtr.Zero, IntPtr.Zero, 0x0002, 5000, out result) == IntPtr.Zero) return -1;
    return result.ToInt32();
  }
  public static bool SetCheck(IntPtr dialog, int id, bool on) {
    IntPtr item = GetDlgItem(dialog, id), result;
    if (item == IntPtr.Zero) return false;
    return SendMessageTimeoutW(item, 0x00F1, (IntPtr) (on ? 1 : 0), IntPtr.Zero, 0x0002, 5000, out result) != IntPtr.Zero;
  }
  // BM_CLICK, posted to the button.
  public static bool Click(IntPtr dialog, int id) {
    IntPtr item = GetDlgItem(dialog, id);
    return item != IntPtr.Zero && PostMessageW(item, 0x00F5, IntPtr.Zero, IntPtr.Zero);
  }
  // WM_COMMAND (id, BN_CLICKED) from the button, posted to the dialog.
  public static bool Command(IntPtr dialog, int id) {
    IntPtr item = GetDlgItem(dialog, id);
    return item != IntPtr.Zero && PostMessageW(dialog, 0x0111, (IntPtr) (id & 0xFFFF), item);
  }
}
'@
}

# The texts of $Key in every language of ui-messages.json (an empty list when
# the catalog or the key is missing: dialogs are then found by their controls).
function Get-GesherUiMessageTexts([string] $Key, [string] $CatalogPath = '') {
  $candidates = @()
  if ($CatalogPath) { $candidates += $CatalogPath }
  $candidates += (Join-Path $PSScriptRoot '..\..\..\tools\package\ui-messages.json')
  if ($env:LOCALAPPDATA) { $candidates += (Join-Path $env:LOCALAPPDATA 'Programs\Gesher\ui-messages.json') }
  foreach ($path in $candidates) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    $catalog = [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8) | ConvertFrom-Json
    $entry = $catalog.messages.PSObject.Properties[$Key]
    if ($null -eq $entry) { return @() }
    return @($entry.Value.PSObject.Properties | ForEach-Object { [string]$_.Value } | Where-Object { $_ })
  }
  return @()
}

# Which of the two dialogs $Hwnd is, by its controls: 'Done' (Open, 1002),
# 'Confirm' (Install/Update, IDOK, and Cancel), or $null.
function Get-GesherInstallDialogKind([IntPtr] $Hwnd) {
  if (-not [GesherInstallDialogNative]::IsWindow($Hwnd) -or -not [GesherInstallDialogNative]::IsWindowVisible($Hwnd)) { return $null }
  if ([GesherInstallDialogNative]::ClassName($Hwnd) -eq 'GesherSplash') { return $null }
  $cancel = [GesherInstallDialogNative]::Shown($Hwnd, $script:GesherInstallDialogIdCancel)
  if ($cancel -and [GesherInstallDialogNative]::Shown($Hwnd, $script:GesherInstallDialogIdOpen)) { return 'Done' }
  if ($cancel -and [GesherInstallDialogNative]::Shown($Hwnd, $script:GesherInstallDialogIdOk)) { return 'Confirm' }
  return $null
}

# The first install dialog of kind $Kind ('Confirm' or 'Done') of process
# $ProcessId (and, unless -NoChildren, of its descendants), or [IntPtr]::Zero.
function Find-GesherInstallDialog([int] $ProcessId, [ValidateSet('Confirm', 'Done')] [string] $Kind, [switch] $NoChildren) {
  $pids = if ($NoChildren) { [uint32[]]@($ProcessId) } else { [GesherInstallDialogNative]::Tree([uint32]$ProcessId) }
  foreach ($h in [GesherInstallDialogNative]::Windows($pids)) {
    if ((Get-GesherInstallDialogKind $h) -eq $Kind) { return $h }
  }
  return [IntPtr]::Zero
}

# Waits up to $TimeoutSeconds for an install dialog of kind $Kind. Returns the
# window, or [IntPtr]::Zero when it did not come (or $Process ended first).
function Wait-GesherInstallDialog([int] $ProcessId, [string] $Kind, [int] $TimeoutSeconds = 300,
    [Diagnostics.Process] $Process = $null, [switch] $NoChildren) {
  $clock = [Diagnostics.Stopwatch]::StartNew()
  while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
    $h = Find-GesherInstallDialog $ProcessId $Kind -NoChildren:$NoChildren
    if ($h -ne [IntPtr]::Zero) { return $h }
    if ($null -ne $Process -and $Process.HasExited) { return [IntPtr]::Zero }
    Start-Sleep -Milliseconds 200
  }
  return [IntPtr]::Zero
}

# Presses button $Id of dialog $Hwnd and waits until the dialog is gone (or no
# longer the same kind of dialog). Returns $true when it went.
function Invoke-GesherDialogButton([IntPtr] $Hwnd, [int] $Id, [int] $TimeoutSeconds = 15) {
  $kind = Get-GesherInstallDialogKind $Hwnd
  if (-not [GesherInstallDialogNative]::Click($Hwnd, $Id)) { return $false }
  $clock = [Diagnostics.Stopwatch]::StartNew()
  $fallback = $false
  while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
    if ((Get-GesherInstallDialogKind $Hwnd) -ne $kind) { return $true }
    if (-not $fallback -and $clock.Elapsed.TotalSeconds -ge 3) {
      $fallback = $true
      [void][GesherInstallDialogNative]::Command($Hwnd, $Id)
    }
    Start-Sleep -Milliseconds 100
  }
  return $false
}

# Sets the desktop shortcut check box to $On, as a click would. Returns $true
# when BM_GETCHECK then reads the wanted state.
function Set-GesherDesktopShortcutBox([IntPtr] $Hwnd, [bool] $On) {
  $id = $script:GesherInstallDialogIdDesktop
  $want = if ($On) { 1 } else { 0 }
  if ([GesherInstallDialogNative]::Check($Hwnd, $id) -eq $want) { return $true }
  [void][GesherInstallDialogNative]::Click($Hwnd, $id)
  for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Milliseconds 100
    if ([GesherInstallDialogNative]::Check($Hwnd, $id) -eq $want) { return $true }
  }
  # Not toggled by BM_CLICK: set it and send the click's notification.
  [void][GesherInstallDialogNative]::SetCheck($Hwnd, $id, $On)
  [void][GesherInstallDialogNative]::Command($Hwnd, $id)
  Start-Sleep -Milliseconds 300
  return [GesherInstallDialogNative]::Check($Hwnd, $id) -eq $want
}

# Answers the dialogs of an APK opened from Explorer, as a user would:
#   -Answer Install|Cancel   the confirmation dialog's button
#   -DesktopShortcut         check the "Create a desktop shortcut" box first
#   -ExpectDesktopBox        $true: the box must be shown (and unchecked by
#                            default); $false: it must not be shown; omitted:
#                            not checked
#   -Done Open|Done|None     the installed dialog's button (None: do not wait
#                            for it, e.g. when the install is expected to fail)
#   -BeforeAnswer           run a test assertion while the confirmation is open
# Returns @{ Problems = @(...); ConfirmTitle; DoneTitle; DesktopBox }. Problems
# is empty when every step happened as asked.
function Invoke-GesherInstallConfirm {
  param(
    [Parameter(Mandatory)] [int] $ProcessId,
    [ValidateSet('Install', 'Cancel')] [string] $Answer = 'Install',
    [switch] $DesktopShortcut,
    $ExpectDesktopBox = $null,
    [ValidateSet('Open', 'Done', 'None')] [string] $Done = 'Done',
    [Diagnostics.Process] $Process = $null,
    [int] $ConfirmTimeoutSeconds = 300,
    [int] $InstallTimeoutSeconds = 600,
    [string] $CatalogPath = '',
    [switch] $NoChildren,
    [scriptblock] $BeforeAnswer
  )
  $result = [pscustomobject]@{ Problems = @(); ConfirmTitle = $null; DoneTitle = $null; DesktopBox = $null }
  $confirm = Wait-GesherInstallDialog $ProcessId 'Confirm' $ConfirmTimeoutSeconds $Process -NoChildren:$NoChildren
  if ($confirm -eq [IntPtr]::Zero) {
    $result.Problems += $(if ($null -ne $Process -and $Process.HasExited) { "no confirmation dialog (gesher-launch exited with code $($Process.ExitCode))" }
                          else { "no confirmation dialog within $ConfirmTimeoutSeconds s" })
    return $result
  }
  $result.ConfirmTitle = [GesherInstallDialogNative]::Title($confirm)
  $titles = @(Get-GesherUiMessageTexts 'install_confirm_title' $CatalogPath)
  if ($titles.Count -and $titles -notcontains $result.ConfirmTitle) {
    $result.Problems += "the confirmation dialog's title is '$($result.ConfirmTitle)', not install_confirm_title ($($titles -join ' / '))"
  }
  $box = [GesherInstallDialogNative]::Shown($confirm, $script:GesherInstallDialogIdDesktop)
  $result.DesktopBox = $box
  if ($null -ne $ExpectDesktopBox) {
    if ([bool]$ExpectDesktopBox -and -not $box) { $result.Problems += 'the confirmation dialog shows no desktop shortcut check box (1001)' }
    if (-not [bool]$ExpectDesktopBox -and $box) { $result.Problems += 'the confirmation dialog shows a desktop shortcut check box (1001) it must not show' }
  }
  if ($box -and [GesherInstallDialogNative]::Check($confirm, $script:GesherInstallDialogIdDesktop) -ne 0) {
    $result.Problems += 'the desktop shortcut check box (1001) is checked by default'
  }
  if ($DesktopShortcut) {
    if (-not $box) { $result.Problems += 'asked to check the desktop shortcut box, but the dialog does not show it' }
    elseif (-not (Set-GesherDesktopShortcutBox $confirm $true)) { $result.Problems += 'the desktop shortcut check box (1001) could not be checked' }
  }
  if ($BeforeAnswer -and -not $result.Problems.Count) {
    try { & $BeforeAnswer | Out-Null }
    catch { $result.Problems += "while awaiting confirmation: $_" }
  }
  if ($result.Problems.Count) {
    # Leave nothing installed by a dialog that is not what was expected.
    [void](Invoke-GesherDialogButton $confirm $script:GesherInstallDialogIdCancel)
    return $result
  }
  $button = if ($Answer -eq 'Install') { $script:GesherInstallDialogIdOk } else { $script:GesherInstallDialogIdCancel }
  if (-not (Invoke-GesherDialogButton $confirm $button)) {
    $result.Problems += "the confirmation dialog did not close after $Answer (BM_CLICK on $button)"
    return $result
  }
  if ($Answer -eq 'Cancel' -or $Done -eq 'None') { return $result }
  $installed = Wait-GesherInstallDialog $ProcessId 'Done' $InstallTimeoutSeconds $Process -NoChildren:$NoChildren
  if ($installed -eq [IntPtr]::Zero) {
    $result.Problems += $(if ($null -ne $Process -and $Process.HasExited) { "no installed dialog (gesher-launch exited with code $($Process.ExitCode))" }
                          else { "no installed dialog within $InstallTimeoutSeconds s of Install" })
    return $result
  }
  $result.DoneTitle = [GesherInstallDialogNative]::Title($installed)
  $titles = @(Get-GesherUiMessageTexts 'install_done_title' $CatalogPath)
  if ($titles.Count -and $titles -notcontains $result.DoneTitle) {
    $result.Problems += "the installed dialog's title is '$($result.DoneTitle)', not install_done_title ($($titles -join ' / '))"
  }
  $button = if ($Done -eq 'Open') { $script:GesherInstallDialogIdOpen } else { $script:GesherInstallDialogIdCancel }
  if (-not (Invoke-GesherDialogButton $installed $button)) {
    $result.Problems += "the installed dialog did not close after $Done (BM_CLICK on $button)"
  }
  return $result
}

# Watches, until $Process ends (or $TimeoutSeconds), for any install dialog of
# it or its descendants. Returns the titles of the dialogs seen (none: empty).
# For an open that must go straight to the app (the same version installed).
function Watch-GesherNoInstallDialog([Diagnostics.Process] $Process, [int] $TimeoutSeconds = 600) {
  $seen = @()
  $clock = [Diagnostics.Stopwatch]::StartNew()
  while (-not $Process.HasExited -and $clock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
    foreach ($h in [GesherInstallDialogNative]::Windows([GesherInstallDialogNative]::Tree([uint32]$Process.Id))) {
      if (Get-GesherInstallDialogKind $h) {
        $title = [GesherInstallDialogNative]::Title($h)
        if ($seen -notcontains $title) { $seen += $title }
      }
    }
    Start-Sleep -Milliseconds 200
  }
  return $seen
}

# A background watch (its own runspace) for install dialogs of any process
# named $ProcessNames, for callers that run gesher-launch synchronously.
# Stop-GesherInstallDialogWatch returns the titles seen.
function Start-GesherInstallDialogWatch([string[]] $ProcessNames = @('gesher-launch', 'Gesher')) {
  $titles = @(Get-GesherUiMessageTexts 'install_confirm_title') + @(Get-GesherUiMessageTexts 'install_done_title')
  $ps = [PowerShell]::Create()
  $null = $ps.AddScript({
    param($names, $titles, $ids, $state)
    $nameOf = @{}
    while (-not $state.Stop) {
      foreach ($h in [GesherInstallDialogNative]::Windows($null)) {
        $procId = [GesherInstallDialogNative]::ProcessId($h)
        if (-not $nameOf.ContainsKey($procId)) {
          $nameOf[$procId] = try { [Diagnostics.Process]::GetProcessById([int]$procId).ProcessName } catch { '' }
        }
        if ($names -notcontains $nameOf[$procId]) { continue }
        $title = [GesherInstallDialogNative]::Title($h)
        $byTitle = $titles.Count -gt 0 -and $titles -contains $title
        $byControls = [GesherInstallDialogNative]::Shown($h, $ids[1]) -and
          ([GesherInstallDialogNative]::Shown($h, $ids[0]) -or [GesherInstallDialogNative]::Shown($h, $ids[2])) -and
          [GesherInstallDialogNative]::ClassName($h) -ne 'GesherSplash'
        if (($byTitle -or ($titles.Count -eq 0 -and $byControls)) -and $state.Seen -notcontains $title) { [void]$state.Seen.Add($title) }
      }
      Start-Sleep -Milliseconds 200
    }
  }).AddArgument($ProcessNames).AddArgument($titles).AddArgument(@(
      $script:GesherInstallDialogIdOk, $script:GesherInstallDialogIdCancel, $script:GesherInstallDialogIdOpen))
  $state = [hashtable]::Synchronized(@{ Stop = $false; Seen = [Collections.ArrayList]::Synchronized([Collections.ArrayList]::new()) })
  $null = $ps.AddArgument($state)
  return [pscustomobject]@{ PowerShell = $ps; Handle = $ps.BeginInvoke(); State = $state }
}

function Stop-GesherInstallDialogWatch($Watch) {
  $Watch.State.Stop = $true
  try { [void]$Watch.PowerShell.EndInvoke($Watch.Handle) } finally { $Watch.PowerShell.Dispose() }
  return @($Watch.State.Seen)
}

# The value of $Key in a Properties.store file (shortcut.properties), with
# Properties' escapes undone; $null when absent.
function Get-GesherShortcutRecordValue([string] $Path, [string] $Key) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
  foreach ($line in [IO.File]::ReadAllLines($Path, [Text.Encoding]::GetEncoding('ISO-8859-1'))) {
    if ($line -match '^\s*[#!]') { continue }
    $m = [regex]::Match($line, '^\s*' + [regex]::Escape($Key) + '\s*[=:]\s*(.*)$')
    if (-not $m.Success) { continue }
    $value = [regex]::Replace($m.Groups[1].Value, '\\u([0-9a-fA-F]{4})|\\(.)', {
      param($e)
      if ($e.Groups[1].Success) { return [string][char][Convert]::ToInt32($e.Groups[1].Value, 16) }
      switch ($e.Groups[2].Value) { 't' { "`t" } 'n' { "`n" } 'r' { "`r" } 'f' { "`f" } default { $e.Groups[2].Value } }
    })
    return $value
  }
  return $null
}
