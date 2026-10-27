# Windows4 (ADR 0061): actual Application Display objects/listeners, rotation and
# simultaneous task overrides. 51-windows3 supplies the real HWND/log helpers.
if (-not (Get-Command W3Wait -ErrorAction SilentlyContinue)) { throw 'Windows4 requires the windows3 HWND/log helpers' }
. "$PSScriptRoot/../graphics/WindowChecks.ps1"
function W4NativeProbe([string] $Executable, [string] $Pattern, [string] $Description, [string[]] $Arguments = @()) {
  $start = [Diagnostics.ProcessStartInfo]::new("$Dir/$Executable")
  foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  $process = [Diagnostics.Process]::Start($start)
  $stdout = $process.StandardOutput.ReadToEndAsync()
  $stderr = $process.StandardError.ReadToEndAsync()
  $timeout = -not $process.WaitForExit(45000)
  if ($timeout) { $process.Kill($true); $process.WaitForExit() }
  $drained = $stdout.Wait(10000) -and $stderr.Wait(10000)
  $text = if ($drained) { $stdout.Result + $stderr.Result } else { 'output pipe did not close' }
  Write-Host $text
  if (-not $timeout -and $drained -and $process.ExitCode -eq 0 -and $text -notmatch '(?m)^FAIL' -and $text -match $Pattern) {
    Write-Host "PASS  windows4: $Description"
  } else {
    Write-Host "FAIL  windows4: $Description (exit $($process.ExitCode), timeout $timeout, drained $drained)"
    $script:failed++
  }
  $process.Dispose()
}
W4NativeProbe windows4_input_pane_probe.exe '0 windows4 COM input pane check\(s\) failed' 'actual IFrameworkInputPane COM source callbacks, window events, geometry and lifetime'
W4NativeProbe windows4_text_store_probe.exe '0 windows4 TSF store check\(s\) failed' 'TSF ACP locks, document/revision protection and an actual Windows text-service reconversion session'
W4NativeProbe windows4_ime_tsf_probe.exe '0 windows4 IME TSF check\(s\) failed' 'production native IME TSF event bridge, restricted editor teardown and reentrant COM detach'
$w4Bcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$w4Dvm = @("-Xbootclasspath:$w4Bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
$w4Apk = NoDrive "$Dir/winapp.apk"
W4NativeProbe dalvikvm.exe 'Windows4ConfigurationTest all passed' 'actual Configuration merging keeps Hebrew/Arabic/English layout direction at all four task rotations' (@($w4Dvm) + @('-cp', (NoDrive "$Dir/graphics-tests.dex"), 'gesher.Windows4ConfigurationTest'))
W4NativeProbe dalvikvm.exe 'Windows4CursorGeometryTest all passed' 'actual CursorAnchorInfo preserves measured character bounds and explicitly replaces missing geometry without a caret' (@($w4Dvm) + @('-cp', (NoDrive "$Dir/graphics-tests.dex"), 'gesher.Windows4CursorGeometryTest'))
W4NativeProbe dalvikvm.exe 'Windows4InputBindingTest all passed' 'actual input service retains a bound editor across later focus-only notifications and accepts real restarts' (@($w4Dvm) + @('-cp', (NoDrive "$Dir/graphics-tests.dex"), 'gesher.Windows4InputBindingTest'))

function W4Check([string] $Mode, [string] $What, $Problems) {
  if (@($Problems).Count -eq 0) { Write-Host "PASS  windows4: $What ($Mode)" }
  else {
    Write-Host "FAIL  windows4: $What ($Mode): $($Problems -join '; ')"
    $script:failed++
    $script:w4Failed = $true
  }
}

function W4Foreground([string] $Mode, [IntPtr] $Window, [string] $Name,
                      [IntPtr] $Previous = [IntPtr]::Zero) {
  $before = [ImeInput]::GetForegroundWindow()
  $problems = @()
  if ($Previous -ne [IntPtr]::Zero -and $before -ne $Previous) {
    $problems += ('Expected previous foreground HWND 0x{0:x}, got 0x{1:x}' -f $Previous.ToInt64(), $before.ToInt64())
  }
  $focused = [ImeInput]::Foreground($Window)
  $after = [ImeInput]::GetForegroundWindow()
  if (-not $focused -or $after -ne $Window) { $problems += 'The requested HWND is not actually foreground' }
  Write-Host ('INFO  windows4 foreground: {0}, before 0x{1:x}, target 0x{2:x}, after 0x{3:x}' -f $Name, $before.ToInt64(), $Window.ToInt64(), $after.ToInt64())
  W4Check $Mode "$Name has the actual native foreground HWND" $problems
  if ($problems.Count) { throw 'Native foreground precondition failed' }
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

function W4Run([string] $Mode, [string[]] $Arguments, [scriptblock] $Scenario,
               [string] $Title = 'Gesher windows4 display', [string] $Activity = 'DisplayActivity') {
  $script:w4Failed = $false
  $log = Join-Path $env:RUNNER_TEMP "windows4-$(Get-Random).log"
  $quoted = (@($w4Dvm) + $Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
  $start = [Diagnostics.ProcessStartInfo]::new('cmd.exe', "/d /s /c `"`"$Dir/dalvikvm.exe`" $quoted > `"$log`" 2>&1`"")
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $process = [Diagnostics.Process]::Start($start)
  $read = { Get-Content -Raw -LiteralPath $log -ErrorAction SilentlyContinue }.GetNewClosure()
  $window = W3Window $read 0 $Title 100
  $problems = @()
  try {
    if ($window -eq [IntPtr]::Zero) { throw "No first frame of $Activity" }
    W4Foreground $Mode $window $Activity
    if ($Activity -eq 'DisplayActivity' -and -not (W3Wait $read 0 'W4 DisplayActivity buttons turn' 30).Success) { throw 'No first button layout' }
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
  if ($text -notmatch "$Activity onDestroy" -or $text -notmatch 'gesher-app: done') {
    $problems += 'Missing Activity destruction/launcher completion'
  }
  W4Check $Mode "$Activity draws a real window and exits after WM_CLOSE" $problems
  if ($script:w4Failed) { Write-Host $text }
  else { $text -split "`r?`n" | Select-String -Pattern 'W4 |onTopResumedActivityChanged' | ForEach-Object { $_.Line } | Select-Object -Last 30 | Out-Host }
}

function W4Legacy([string] $Mode, [IntPtr] $Window, [scriptblock] $Read) {
  $problems = @()
  $normal = W3Wait $Read 0 'W4 legacy target 28 content 400x800 edit bottom 800 padding 0 ime visible false bottom 0' 40
  if (-not $normal.Success) { $problems += 'No unpadded target-28 initial content layout' }
  $client = [GesherW3Win32]::ClientOnScreen($Window)
  $mark = W3Mark $Read
  [void][GesherW3Win32]::PostMessageW($Window, $w3Input, [IntPtr]($client[3] - 300), [IntPtr]$client[3])
  $resize = W3WaitWhere $Read $mark 'W4 legacy target 28 content 400x(\d+) edit bottom (\d+) padding 0 ime visible true bottom 300' {
    param($groups) [int]$groups[1].Value -eq 500 -and [int]$groups[2].Value -le 500
  } 40
  if (-not $resize.Success) { $problems += 'The framework did not resize unpadded content to 500 and keep the editor above the IME' }
  if ((W3Size $Window) -ne '400x800') { $problems += 'The native client was resized instead of the legacy content' }
  $floatingProblems = @()
  $mark = W3Mark $Read
  [void][GesherW3Win32]::PostMessageW($Window, $w3Input, [IntPtr]($client[1] + 250), [IntPtr]($client[1] + 450))
  if (-not (W3Wait $Read $mark 'W4 legacy target 28 content 400x800 edit bottom 800 padding 0 ime visible true bottom 0' 40).Success) {
    $floatingProblems += 'Undocking the pane did not clear Android edge insets and restore unpadded content'
  }
  if ((W3Size $Window) -ne '400x800') { $floatingProblems += 'The floating pane changed the native client size' }
  W4Check $Mode 'a floating input pane leaves Android IME edge insets empty and target-28 content unresized' $floatingProblems
  $mark = W3Mark $Read
  [void][GesherW3Win32]::PostMessageW($Window, $w3Input, [IntPtr]($client[3] - 300), [IntPtr]$client[3])
  if (-not (W3WaitWhere $Read $mark 'W4 legacy target 28 content 400x(\d+) edit bottom (\d+) padding 0 ime visible true bottom 300' {
    param($groups) [int]$groups[1].Value -eq 500 -and [int]$groups[2].Value -le 500
  } 40).Success) { $problems += 'Docking again did not restore the framework content resize' }
  $mark = W3Mark $Read
  [void][GesherW3Win32]::PostMessageW($Window, $w3Input, [IntPtr]::Zero, [IntPtr]::Zero)
  if (-not (W3Wait $Read $mark 'W4 legacy target 28 content 400x800 edit bottom 800 padding 0 ime visible false bottom 0' 40).Success) {
    $problems += 'Legacy content did not restore after hiding IME occlusion'
  }
  W4Check $Mode 'target-28 adjustResize shrinks/restores framework content without app padding or native-window resizing' $problems
}

function W4Tsf([string] $Mode, [IntPtr] $Window, [scriptblock] $Read) {
  $buttons = W3Wait $Read 0 'W4 TSF buttons reconvert (\d+),(\d+) password (\d+),(\d+) number (\d+),(\d+) normal (\d+),(\d+) partial (\d+),(\d+) noneditor (\d+),(\d+)' 40
  if (-not $buttons.Success) { throw 'No TSF fixture buttons' }
  $problems = @()
  foreach ($step in @(@('reconvert', 1), @('password', 3), @('number', 5), @('normal', 7), @('reconvert', 1), @('noneditor', 11), @('partial', 9))) {
    $mark = W3Mark $Read
    $group = [int]$step[1]
    [GesherWin32]::Click($Window, [int]$buttons.Groups[$group].Value, [int]$buttons.Groups[$group + 1].Value)
    $pattern = switch ($step[0]) {
      reconvert { 'W4 TSF actual EditText Japanese true composition ended true selection 9-9' }
      password { 'W4 TSF restricted password HRESULT 8000ffff unchanged true' }
      number { 'W4 TSF restricted number HRESULT 8000ffff unchanged true' }
      normal { 'W4 TSF normal selected Hebrew 7-11' }
      noneditor { 'W4 TSF noneditor focused true accepting false HRESULT 8000ffff unchanged true' }
      partial { 'W4 TSF actual EditText partial true composition ended true selection 2-2' }
    }
    if (-not (W3Wait $Read $mark $pattern 25).Success) { $problems += "Missing $($step[0]) result: $pattern" }
    $live = switch ($step[0]) {
      reconvert { 'W4 TSF live composing 7-9 Japanese true' }
      partial { 'W4 TSF live composing 0-4 Japanese false partial true' }
      default { $null }
    }
    if ($live -and -not (W3Wait $Read $mark $live 10).Success) { $problems += "Missing actual composing spans: $live" }
  }
  if ("$(& $Read)" -notmatch 'W4 TSF service HRESULT 0') { $problems += 'No successful actual Windows text-service edit session' }
  W4Check $Mode 'real TSF service reconverts existing EditText selection, completes composition, blocks password/number/editorless focus, and reconnects normal editor' $problems
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

$w4LegacyApk = NoDrive "$Dir/winlegacy.apk"
$w4PreviousService = $env:GESHER_TSF_TEST_SERVICE
$env:GESHER_TSF_TEST_SERVICE = [IO.Path]::GetFullPath("$Dir/windows4_tsf_service.dll")
try {
  W4Run 'single process, TSF' @('com.gesher.app.Main', '--window', '--display', '400x800@160', '--activity', 'com.gesher.winapp.TsfActivity', $w4Apk) {
    param($window, $read)
    W4Tsf 'single process' $window $read
  } -Title 'Gesher windows4 TSF' -Activity TsfActivity
} finally {
  if ($w4PreviousService) { $env:GESHER_TSF_TEST_SERVICE = $w4PreviousService }
  else { Remove-Item Env:GESHER_TSF_TEST_SERVICE -ErrorAction SilentlyContinue }
}
W4Run 'single process, legacy resize' @('com.gesher.app.Main', '--window', '--display', '400x800@160', $w4LegacyApk) {
  param($window, $read)
  W4Legacy 'single process' $window $read
} -Title 'Gesher windows4 legacy' -Activity LegacyActivity

$w4Data = $env:ANDROID_DATA
$w4Logs = $env:GESHER_LOGS
$w4Shortcuts = $env:GESHER_SHORTCUTS
$env:ANDROID_DATA = NoDrive "$root/data-windows4"
$env:GESHER_LOGS = NoDrive "$root/logs-windows4"
$env:GESHER_SHORTCUTS = '0'
New-Item -ItemType Directory -Force "$root/data-windows4" | Out-Null
try {
  $env:GESHER_TSF_TEST_SERVICE = [IO.Path]::GetFullPath("$Dir/windows4_tsf_service.dll")
  W4Run 'device, TSF' @('com.gesher.app.Launcher', 'launch', $w4Apk, '--window', '--activity', 'com.gesher.winapp.TsfActivity') {
    param($window, $read)
    W4Tsf device $window $read
  } -Title 'Gesher windows4 TSF' -Activity TsfActivity
  W4Run 'device, two visible tasks' @('com.gesher.app.Launcher', 'launch', $w4Apk, '--window', '--activity', 'com.gesher.winapp.DisplayActivity') {
    param($window, $read)
    W4Snapshot device $read 0 DisplayActivity 0 0 $true
    $at = W4Point $read DisplayActivity task
    $mark = W3Mark $read
    [GesherWin32]::Click($window, $at[0], $at[1])
    $land = W3Window $read $mark 'Gesher windows4 landscape' 90
    try {
      if ($land -eq [IntPtr]::Zero) { throw 'No second task window' }
      # Showing a new HWND does not guarantee host foreground on every runner.
      # Establish it explicitly before requiring a real landscape->portrait transition.
      W4Foreground device $land 'the landscape task before the first shared-display snapshot'
      W4Snapshot device $read $mark DisplayLandActivity 90 90 $true
      W4Snapshot device $read $mark DisplayActivity 90 0 $false 1
      $mark = W3Mark $read
      W4Foreground device $window 'the portrait task after the landscape task' $land
      W4Snapshot device $read $mark DisplayActivity 0 0 $true 2
      W4Snapshot device $read $mark DisplayLandActivity 0 90 $false 1
      $mark = W3Mark $read
      W4Foreground device $land 'the landscape task after the portrait task' $window
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
  W4Run 'device, legacy resize' @('com.gesher.app.Launcher', 'launch', $w4LegacyApk, '--window') {
    param($window, $read)
    W4Legacy device $window $read
  } -Title 'Gesher windows4 legacy' -Activity LegacyActivity
} finally {
  if ($w4PreviousService) { $env:GESHER_TSF_TEST_SERVICE = $w4PreviousService }
  else { Remove-Item Env:GESHER_TSF_TEST_SERVICE -ErrorAction SilentlyContinue }
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
