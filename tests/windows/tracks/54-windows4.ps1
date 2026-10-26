# Windows4 (ADR 0061): actual Application Display objects/listeners, rotation and
# simultaneous task overrides. 51-windows3 supplies the real HWND/log helpers.
if (-not (Get-Command W3Wait -ErrorAction SilentlyContinue)) { throw 'Windows4 requires the windows3 HWND/log helpers' }
. "$PSScriptRoot/../graphics/WindowChecks.ps1"
$w4Bcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$w4Dvm = @("-Xbootclasspath:$w4Bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
$w4Apk = NoDrive "$Dir/winapp.apk"

function W4Check([string] $Mode, [string] $What, $Problems) {
  if (@($Problems).Count -eq 0) { Write-Host "PASS  windows4: $What ($Mode)" }
  else {
    Write-Host "FAIL  windows4: $What ($Mode): $($Problems -join '; ')"
    $script:failed++
    $script:w4Failed = $true
  }
}

function W4Snapshot([string] $Mode, [scriptblock] $Read, [int] $Mark, [string] $Name,
                    [int] $Shared, [int] $Own, [bool] $Top, [int] $Callbacks = 0) {
  $sharedSize = if ($Shared -in 90,270) { '800x400' } else { '400x800' }
  $ownSize = if ($Own -in 90,270) { '800x400' } else { '400x800' }
  $topText = "$Top".ToLowerInvariant()
  $pattern = "W4 $Name shared $Shared size $sharedSize real $sharedSize fresh $Shared size $sharedSize real $sharedSize own $Own size $ownSize real $ownSize top $topText callbacks (\d+)"
  $condition = { param($groups) [int]$groups[1].Value -ge $Callbacks }.GetNewClosure()
  $seen = W3WaitWhere $Read $Mark $pattern $condition 40
  $problems = @()
  if (-not $seen.Success) { $problems += "missing snapshot '$pattern' with at least $Callbacks callbacks" }
  W4Check $Mode "$Name held/fresh Application Display rotation $Shared and Activity rotation $Own, top $topText, listener callbacks >= $Callbacks" $problems
}

function W4Point([scriptblock] $Read, [string] $Name, [string] $Which) {
  $all = [regex]::Matches("$(& $Read)", "W4 $Name buttons turn (\d+),(\d+) task (\d+),(\d+)")
  if ($all.Count -eq 0) { throw "No button layout for $Name" }
  $offset = if ($Which -eq 'turn') { 1 } else { 3 }
  $last = $all[$all.Count - 1]
  return @([int]$last.Groups[$offset].Value, [int]$last.Groups[$offset + 1].Value)
}

function W4Run([string] $Mode, [string[]] $Arguments, [scriptblock] $Scenario) {
  $script:w4Failed = $false
  $log = Join-Path $env:RUNNER_TEMP "windows4-$(Get-Random).log"
  $quoted = (@($w4Dvm) + $Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
  $start = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"`"$Dir/dalvikvm.exe`" $quoted > `"$log`" 2>&1`"")
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $process = [Diagnostics.Process]::Start($start)
  $read = { Get-Content -Raw -LiteralPath $log -ErrorAction SilentlyContinue }.GetNewClosure()
  $window = W3Window $read 0 'Gesher windows4 display' 100
  $problems = @()
  try {
    if ($window -eq [IntPtr]::Zero) { throw 'No first frame of DisplayActivity' }
    if (-not (W3Wait $read 0 'W4 DisplayActivity buttons turn' 30).Success) { throw 'No first button layout' }
    & $Scenario $window $read
  } catch { $problems += "scenario: $_ $($_.ScriptStackTrace)" }
  finally {
    if ($window -ne [IntPtr]::Zero) {
      [void][GesherWin32]::PostMessageW($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    if (-not $process.WaitForExit(120000)) { $process.Kill($true); $problems += 'Did not exit after WM_CLOSE' }
    elseif ($process.ExitCode -ne 0) { $problems += "Process exit $($process.ExitCode)" }
  }
  $text = "$(& $read)"
  if ($text -notmatch 'DisplayActivity onDestroy' -or $text -notmatch 'gesher-app: done') {
    $problems += 'Missing Activity destruction/launcher completion'
  }
  W4Check $Mode 'Display fixture draws a real window and exits after WM_CLOSE' $problems
  if ($script:w4Failed) { Write-Host $text }
  else { $text -split "`r?`n" | Select-String -Pattern 'W4 |onTopResumedActivityChanged' | ForEach-Object { $_.Line } | Select-Object -Last 30 | Out-Host }
}

W4Run 'single process' @('com.gesher.app.Main', '--window', '--display', '400x800@160', '--activity', 'com.gesher.winapp.DisplayActivity', $w4Apk) {
  param($window, $read)
  W4Snapshot 'single process' $read 0 DisplayActivity 0 0 $true
  $at = W4Point $read DisplayActivity turn
  $mark = W3Mark $read
  [GesherWin32]::Click($window, $at[0], $at[1])
  W4Snapshot 'single process' $read $mark DisplayActivity 90 90 $true 1
  $at = W4Point $read DisplayActivity turn
  $mark = W3Mark $read
  [GesherWin32]::Click($window, $at[0], $at[1])
  W4Snapshot 'single process' $read $mark DisplayActivity 0 0 $true 2
}

$w4Data = $env:ANDROID_DATA
$w4Logs = $env:GESHER_LOGS
$w4Shortcuts = $env:GESHER_SHORTCUTS
$env:ANDROID_DATA = NoDrive "$root/data-windows4"
$env:GESHER_LOGS = NoDrive "$root/logs-windows4"
$env:GESHER_SHORTCUTS = '0'
New-Item -ItemType Directory -Force "$root/data-windows4" | Out-Null
try {
  W4Run 'device, two visible tasks' @('com.gesher.app.Launcher', 'launch', $w4Apk, '--window', '--activity', 'com.gesher.winapp.DisplayActivity') {
    param($window, $read)
    W4Snapshot device $read 0 DisplayActivity 0 0 $true
    $at = W4Point $read DisplayActivity task
    $mark = W3Mark $read
    [GesherWin32]::Click($window, $at[0], $at[1])
    $land = W3Window $read $mark 'Gesher windows4 landscape' 90
    try {
      if ($land -eq [IntPtr]::Zero) { throw 'No second task window' }
      W4Snapshot device $read $mark DisplayLandActivity 90 90 $true
      W4Snapshot device $read $mark DisplayActivity 90 0 $false 1
      $mark = W3Mark $read
      if (-not [ImeInput]::Foreground($window)) { throw 'Could not focus the portrait HWND' }
      W4Snapshot device $read $mark DisplayActivity 0 0 $true 2
      W4Snapshot device $read $mark DisplayLandActivity 0 90 $false 1
      $mark = W3Mark $read
      if (-not [ImeInput]::Foreground($land)) { throw 'Could not focus the landscape HWND' }
      W4Snapshot device $read $mark DisplayLandActivity 90 90 $true 2
      W4Snapshot device $read $mark DisplayActivity 90 0 $false 3
      if (-not [GesherWin32]::IsWindowVisible($window) -or -not [GesherWin32]::IsWindowVisible($land)) {
        throw 'The portrait and landscape tasks were not simultaneously visible'
      }
    } finally {
      if ($land -ne [IntPtr]::Zero) {
        $mark = W3Mark $read
        [void][GesherWin32]::PostMessageW($land, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        if (-not (W3Wait $read $mark 'DisplayLandActivity onDestroy' 40).Success) { throw 'Second task did not finish' }
      }
    }
    W4Snapshot device $read $mark DisplayActivity 0 0 $true 4
  }
} finally {
  $shutdown = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
  foreach ($argument in @($w4Dvm) + @('com.gesher.app.Launcher','shutdown')) { $shutdown.ArgumentList.Add($argument) }
  $shutdown.UseShellExecute = $false
  $shutdown.CreateNoWindow = $true
  $stop = [Diagnostics.Process]::Start($shutdown)
  if (-not $stop.WaitForExit(120000)) { $stop.Kill($true); Write-Host 'FAIL  windows4: device shutdown timed out'; $script:failed++ }
  $env:ANDROID_DATA = $w4Data
  if ($w4Logs) { $env:GESHER_LOGS = $w4Logs } else { Remove-Item Env:GESHER_LOGS -ErrorAction SilentlyContinue }
  if ($w4Shortcuts) { $env:GESHER_SHORTCUTS = $w4Shortcuts } else { Remove-Item Env:GESHER_SHORTCUTS -ErrorAction SilentlyContinue }
}
