param([Parameter(Mandatory)][string] $Executable, [string] $ControllerFolder = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/NativeForeground.ps1"
$Executable = (Resolve-Path -LiteralPath $Executable).Path

function Wait-ForegroundReceipt($Helper, [string] $Path) {
  $deadline = [DateTime]::UtcNow.AddSeconds(15)
  while (-not (Test-Path -LiteralPath $Path) -and -not $Helper.Process.HasExited -and
         [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 20 }
  if (-not (Test-Path -LiteralPath $Path)) { throw 'Native helper did not publish its receipt in 15 seconds' }
  Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

# Explicit isolated control: this parent exits without requesting child shutdown.
# The normal helper must observe the retained parent handle and retire itself.
if ($ControllerFolder) {
  $helper = Start-GesherForegroundHelper $Executable (Join-Path $ControllerFolder 'window.json') `
    (Join-Path $ControllerFolder 'stop') $ControllerFolder
  [void](Wait-ForegroundReceipt $helper (Join-Path $ControllerFolder 'window.json'))
  $deadline = [DateTime]::UtcNow.AddSeconds(15)
  while (-not (Test-Path -LiteralPath (Join-Path $ControllerFolder 'exit-parent')) -and
         [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 20 }
  if (-not (Test-Path -LiteralPath (Join-Path $ControllerFolder 'exit-parent'))) {
    [void](Stop-GesherForegroundHelper $helper)
    exit 1
  }
  exit 0
}

# This compilation precedes every timed child launch; the child is already native.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NativeForegroundChecks {
  [StructLayout(LayoutKind.Sequential)] struct FileTime { public uint Low, High; }
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr window);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr window, uint message, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr window, int command);
  [DllImport("user32.dll", SetLastError = true)] static extern IntPtr SendMessageTimeoutW(IntPtr window, uint message, UIntPtr w, IntPtr l, uint flags, uint timeout, out UIntPtr result);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
  [DllImport("kernel32.dll")] static extern bool GetProcessTimes(IntPtr process, out FileTime created, out FileTime exited, out FileTime kernel, out FileTime user);
  public static uint Owner(IntPtr window) { uint pid; GetWindowThreadProcessId(window, out pid); return pid; }
  public static ulong Generation(System.Diagnostics.Process process) {
    FileTime c, e, k, u;
    if (!GetProcessTimes(process.Handle, out c, out e, out k, out u)) throw new InvalidOperationException("GetProcessTimes failed");
    return ((ulong)c.High << 32) | c.Low;
  }
  public static bool SystemCommand(IntPtr window, uint command, int argument) {
    UIntPtr result;
    return SendMessageTimeoutW(window, 0x112, new UIntPtr(command), new IntPtr(argument),
        3 /* SMTO_BLOCK | SMTO_ABORTIFHUNG */, 1000, out result) != IntPtr.Zero;
  }
}
'@
Add-Type -Path "$PSScriptRoot/../ime/ImeInput.cs"

$checks = 0
function Assert-Foreground([bool] $Okay, [string] $Reason) {
  $script:checks++
  if (-not $Okay) { throw $Reason }
}

# A PID read from a receipt does not confer cleanup ownership. Keep the process
# handle unowned until both its creation identity and actual HWND owner match.
# A rejected handle is disposed only; it must never reach termination cleanup.
function Get-VerifiedForegroundProcess($Receipt) {
  $candidate = $null
  try {
    Assert-Foreground ($Receipt.pid -gt 0 -and $Receipt.pid -ne $PID -and $Receipt.created -gt 0) 'Invalid foreground receipt identity'
    $candidate = [Diagnostics.Process]::GetProcessById([int]$Receipt.pid)
    Assert-Foreground ([NativeForegroundChecks]::Generation($candidate) -eq [ulong]$Receipt.created) 'Foreground receipt creation time differs'
    $window = [IntPtr][long]$Receipt.hwnd
    Assert-Foreground ([NativeForegroundChecks]::IsWindow($window) -and [NativeForegroundChecks]::Owner($window) -eq $candidate.Id) 'Foreground receipt HWND ownership differs'
    Assert-Foreground (-not $candidate.HasExited) 'Foreground receipt process already exited'
    $verified = $candidate
    $candidate = $null
    return $verified
  } finally {
    if ($candidate) { $candidate.Dispose() }
  }
}
$temporary = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$root = Join-Path $temporary "clipboard-foreground-contract-$([Guid]::NewGuid().ToString('N'))"
[void][IO.Directory]::CreateDirectory($root)
Write-Host "CLIPBOARD_HELPER_BINARY sha256=$((Get-FileHash -LiteralPath $Executable).Hash.ToLowerInvariant())"

for ($iteration = 0; $iteration -lt 4; $iteration++) {
  # Spaces and non-ASCII names verify that paths are passed as arguments, not shell text.
  $folder = Join-Path $root "native window $iteration-é"
  [void][IO.Directory]::CreateDirectory($folder)
  $receiptPath = Join-Path $folder 'window.json'
  $helper = Start-GesherForegroundHelper $Executable $receiptPath (Join-Path $folder 'stop') $folder
  $window = [IntPtr]::Zero
  try {
    $receipt = Wait-ForegroundReceipt $helper $receiptPath
    $window = [IntPtr][long]$receipt.hwnd
    Assert-Foreground (-not $helper.Process.HasExited) 'Helper exited before identity validation'
    Assert-Foreground ($receipt.pid -eq $helper.Process.Id -and $receipt.pid -ne $PID) 'Receipt must name the owned foreign process'
    Assert-Foreground ([NativeForegroundChecks]::Generation($helper.Process) -eq [ulong]$receipt.created) 'Receipt must match actual process creation time'
    Assert-Foreground ([NativeForegroundChecks]::Owner($window) -eq $helper.Process.Id) 'Receipt must match actual native HWND owner'
    Assert-Foreground ([NativeForegroundChecks]::IsWindow($window) -and -not [NativeForegroundChecks]::IsWindowVisible($window)) 'Setup must create a real hidden window without stealing focus'
  } finally { $problems = @(Stop-GesherForegroundHelper $helper) }
  Assert-Foreground ($problems.Count -eq 0) "Normal retirement failed: $problems"
  Assert-Foreground (-not [NativeForegroundChecks]::IsWindow($window)) 'Owned HWND survived normal retirement'
  $stdout = Get-Content -LiteralPath (Join-Path $folder 'stdout.log') -Raw
  Assert-Foreground ($stdout -match 'phase=receipt_ready' -and $stdout -match 'phase=window_destroyed' -and $stdout -match 'phase=class_unregistered' -and $stdout -match 'phase=complete') 'Missing native setup/cleanup evidence'
}

# The application test really foregrounds the window through an Alt-assisted
# helper. A default window procedure can then enter a nested menu/move/size loop
# and stop checking its outer-loop stop file. Exercise each command on an actual
# visible foreground HWND, require the synchronous command to return promptly,
# then require the same unchanged graceful-retirement bound and cleanup phases.
foreach ($command in @(0xF100, 0xF090, 0xF010, 0xF000)) {
  $folder = Join-Path $root "foreground-modal-$command"
  [void][IO.Directory]::CreateDirectory($folder)
  $receiptPath = Join-Path $folder 'window.json'
  $helper = Start-GesherForegroundHelper $Executable $receiptPath (Join-Path $folder 'stop') $folder
  $window = [IntPtr]::Zero
  try {
    $receipt = Wait-ForegroundReceipt $helper $receiptPath
    $window = [IntPtr][long]$receipt.hwnd
    Assert-Foreground ([NativeForegroundChecks]::Generation($helper.Process) -eq [ulong]$receipt.created -and [NativeForegroundChecks]::Owner($window) -eq $helper.Process.Id) 'Modal control must use the owned HWND generation'
    [void][NativeForegroundChecks]::ShowWindowAsync($window, 5)
    Assert-Foreground ([ImeInput]::Foreground($window) -and [NativeForegroundChecks]::IsWindowVisible($window)) 'Modal control needs the actual visible native foreground'
    Assert-Foreground ([ImeInput]::Keys([ushort]0x12) -eq 2) 'Could not send real Alt press/release to the owned foreground'
    # Space requests the actual system menu for SC_KEYMENU. The other commands
    # normally start mouse menu/moving/sizing modal processing in DefWindowProc.
    Assert-Foreground ([NativeForegroundChecks]::SystemCommand($window, $command, 32)) "System command $command entered a blocking modal loop"
    Assert-Foreground ([ImeInput]::GetForegroundWindow() -eq $window -and -not $helper.Process.HasExited) 'Modal command changed owned foreground or process liveness'
  } finally { $problems = @(Stop-GesherForegroundHelper $helper) }
  Assert-Foreground ($problems.Count -eq 0) "Foreground/modal retirement failed: $problems"
  Assert-Foreground (-not [NativeForegroundChecks]::IsWindow($window)) 'Modal control HWND survived retirement'
  $stdout = Get-Content -LiteralPath (Join-Path $folder 'stdout.log') -Raw
  Assert-Foreground ($stdout -match 'phase=modal_command_ignored' -and $stdout -match 'phase=stop_received' -and $stdout -match 'phase=window_destroyed' -and $stdout -match 'phase=complete') 'Missing modal-command/normal-retirement evidence'
}

# A live sentinel models a PID reused after a stale receipt was written. The
# real validation path must reject the mismatched generation without killing it.
$sentinelFolder = Join-Path $root 'stale-generation-sentinel'
[void][IO.Directory]::CreateDirectory($sentinelFolder)
$sentinelReceipt = Join-Path $sentinelFolder 'window.json'
$sentinel = Start-GesherForegroundHelper $Executable $sentinelReceipt (Join-Path $sentinelFolder 'stop') $sentinelFolder
try {
  $receipt = Wait-ForegroundReceipt $sentinel $sentinelReceipt
  $stale = [pscustomobject]@{ pid = $receipt.pid; created = [ulong]$receipt.created + [ulong]1; hwnd = $receipt.hwnd }
  $claimed = $null
  $rejected = $false
  try { $claimed = Get-VerifiedForegroundProcess $stale }
  catch { $rejected = $_.Exception.Message -match 'creation time differs' }
  finally { if ($claimed) { $claimed.Dispose() } }
  Assert-Foreground $rejected 'Stale creation identity was not rejected by the cleanup-ownership boundary'
  $sentinel.Process.Refresh()
  Assert-Foreground (-not $sentinel.Process.HasExited) 'Rejected receipt terminated the live sentinel'
  Assert-Foreground ([NativeForegroundChecks]::Generation($sentinel.Process) -eq [ulong]$receipt.created) 'Sentinel process identity changed during rejection'
  Assert-Foreground ([NativeForegroundChecks]::IsWindow([IntPtr][long]$receipt.hwnd) -and [NativeForegroundChecks]::Owner([IntPtr][long]$receipt.hwnd) -eq $sentinel.Process.Id) 'Rejected receipt destroyed the live sentinel HWND'
} finally { $problems = @(Stop-GesherForegroundHelper $sentinel) }
Assert-Foreground ($problems.Count -eq 0) "Sentinel owner could not retire its own helper: $problems"

$badFolder = Join-Path $root 'publish-failure'
[void][IO.Directory]::CreateDirectory($badFolder)
$badReceipt = Join-Path $badFolder 'missing-directory/window.json'
$helper = Start-GesherForegroundHelper $Executable $badReceipt (Join-Path $badFolder 'stop') $badFolder
try {
  Assert-Foreground ($helper.Process.WaitForExit(5000)) 'Invalid receipt path did not fail promptly'
  Assert-Foreground ($helper.Process.ExitCode -ne 0 -and -not (Test-Path -LiteralPath $badReceipt)) 'Receipt publication failure became success'
} finally { $problems = @(Stop-GesherForegroundHelper $helper) }
Assert-Foreground ($problems.Count -gt 0) 'Cleanup must retain failed child exit as a failure'
Assert-Foreground ((Get-Content -LiteralPath (Join-Path $badFolder 'stderr.log') -Raw) -match 'failure=create_receipt') 'Native publication failure needs phase diagnostics'

$closedFolder = Join-Path $root 'unexpected-close'
[void][IO.Directory]::CreateDirectory($closedFolder)
$helper = Start-GesherForegroundHelper $Executable (Join-Path $closedFolder 'window.json') (Join-Path $closedFolder 'stop') $closedFolder
try {
  $receipt = Wait-ForegroundReceipt $helper (Join-Path $closedFolder 'window.json')
  $window = [IntPtr][long]$receipt.hwnd
  Assert-Foreground ([NativeForegroundChecks]::PostMessageW($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)) 'Could not close owned negative-control HWND'
  Assert-Foreground ($helper.Process.WaitForExit(5000)) 'Unexpected window destruction did not terminate helper'
  Assert-Foreground ($helper.Process.ExitCode -ne 0) 'Unexpected window destruction became successful shutdown'
} finally { $problems = @(Stop-GesherForegroundHelper $helper) }
Assert-Foreground ($problems.Count -gt 0) 'Unexpected shutdown must remain a failing result'

$parentFolder = Join-Path $root 'parent-exit'
[void][IO.Directory]::CreateDirectory($parentFolder)
$info = [Diagnostics.ProcessStartInfo]::new()
$info.FileName = Join-Path $PSHOME 'pwsh.exe'
$info.UseShellExecute = $false; $info.CreateNoWindow = $true
foreach ($argument in @('-NoLogo', '-NoProfile', '-NonInteractive', '-File', $PSCommandPath,
    '-Executable', $Executable, '-ControllerFolder', $parentFolder)) { $info.ArgumentList.Add($argument) }
$parent = [Diagnostics.Process]::Start($info)
$child = $null
try {
  $deadline = [DateTime]::UtcNow.AddSeconds(20)
  $receiptPath = Join-Path $parentFolder 'window.json'
  while (-not (Test-Path -LiteralPath $receiptPath) -and -not $parent.HasExited -and
         [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 20 }
  Assert-Foreground (Test-Path -LiteralPath $receiptPath) 'Parent-exit control did not create owned child'
  $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
  $child = Get-VerifiedForegroundProcess $receipt
  $window = [IntPtr][long]$receipt.hwnd
  [IO.File]::WriteAllText((Join-Path $parentFolder 'exit-parent'), 'exit')
  Assert-Foreground ($parent.WaitForExit(5000) -and $parent.ExitCode -eq 0) 'Control parent did not exit normally'
  Assert-Foreground ($child.WaitForExit(5000)) 'Helper outlived its parent process'
  Assert-Foreground (-not [NativeForegroundChecks]::IsWindow($window)) 'HWND survived owned parent death'
} finally {
  if (-not $parent.HasExited) { $parent.Kill($true); [void]$parent.WaitForExit(5000) }
  $parent.Dispose()
  if ($child) {
    if (-not $child.HasExited) { $child.Kill($true); [void]$child.WaitForExit(5000) }
    $child.Dispose()
  }
}
Write-Host "PASS native foreground helper: $checks identity/lifecycle/failure checks"
