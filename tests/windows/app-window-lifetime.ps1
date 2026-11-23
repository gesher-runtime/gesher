# Required Track51 supplement: real manifest processes, native menus and owned HWND lifetime.
# Source/runtime inputs are the same current-build artifacts as the surrounding desktop track.
function Test-AppWindowLifetime {
  if (-not ('GesherAppTaskMenu' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class GesherAppTaskMenu {
  public sealed class Item { public uint Id; public string Label; public bool Checked; }
  [DllImport("user32.dll")] static extern IntPtr GetSystemMenu(IntPtr hwnd, bool revert);
  [DllImport("user32.dll")] static extern int GetMenuItemCount(IntPtr menu);
  [DllImport("user32.dll")] static extern uint GetMenuItemID(IntPtr menu, int position);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetMenuStringW(
      IntPtr menu, uint position, StringBuilder text, int length, uint flags);
  [DllImport("user32.dll")] static extern uint GetMenuState(IntPtr menu, uint position, uint flags);
  public static Item[] Read(IntPtr hwnd) {
    IntPtr menu = GetSystemMenu(hwnd, false); // Borrowed; never destroy another window's menu.
    var result = new List<Item>();
    for (int n = 0, count = GetMenuItemCount(menu); n < count; n++) {
      uint id = GetMenuItemID(menu, n);
      if (id < 0x1000 || id >= 0x1400) continue;
      var text = new StringBuilder(1024);
      GetMenuStringW(menu, (uint)n, text, text.Capacity, 0x400);
      result.Add(new Item { Id = id, Label = text.ToString(),
          Checked = (GetMenuState(menu, (uint)n, 0x400) & 8) != 0 });
    }
    return result.ToArray();
  }
}
'@
  }
  function AWRequire([bool] $Condition, [string] $Reason) {
    if (-not $Condition) { throw $Reason }
  }
  function AWWait([scriptblock] $Condition, [int] $Seconds = 30) {
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
      if (& $Condition) { return $true }
      Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
  }
  $owned = [Collections.Generic.List[Diagnostics.Process]]::new()
  $windows = [Collections.Generic.List[IntPtr]]::new()
  $saved = @{}
  foreach ($name in @('ANDROID_DATA', 'GESHER_LOGS', 'GESHER_SHORTCUTS', 'GESHER_SEPARATE_WINDOWS')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
  }
  $profile = Join-Path $root "data-app-window-lifetime-$(Get-Random)"
  $logs = Join-Path $root "logs-app-window-lifetime-$(Get-Random)"
  [void](New-Item -ItemType Directory -Path $profile, $logs)
  $env:ANDROID_DATA = NoDrive $profile
  $env:GESHER_LOGS = NoDrive $logs
  $env:GESHER_SHORTCUTS = '0'
  Remove-Item Env:GESHER_SEPARATE_WINDOWS -ErrorAction SilentlyContinue
  $read = { Get-Content -Raw -LiteralPath (Join-Path $logs 'com.gesher.winapp.log') -ErrorAction SilentlyContinue }.GetNewClosure()
  $foreignRead = { Get-Content -Raw -LiteralPath (Join-Path $logs 'com.gesher.winlegacy.log') -ErrorAction SilentlyContinue }.GetNewClosure()

  function AWRun([string[]] $Words) {
    $start = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($word in (@($dvmW3) + @('com.gesher.app.Launcher') + $Words)) {
      [void]$start.ArgumentList.Add($word)
    }
    $process = [Diagnostics.Process]::Start($start)
    [void]$owned.Add($process)
    [void]$process.Handle
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    AWRequire ($process.WaitForExit(120000)) "Launcher did not retire: $($Words -join ' ')"
    AWRequire ($stdout.Wait(10000) -and $stderr.Wait(10000)) 'Launcher output did not reach EOF'
    Write-Host ("APP_WINDOW_COMMAND {0} exit={1} {2} {3}" -f ($Words -join ' '), $process.ExitCode, $stdout.Result, $stderr.Result)
    AWRequire ($process.ExitCode -eq 0) "Launcher failed: $($Words -join ' ')"
  }
  function AWRetain([IntPtr] $Window) {
    [uint32]$ownerPid = 0
    AWRequire ([GesherWin32]::GetWindowThreadProcessId($Window, [ref]$ownerPid) -ne 0) 'Native HWND has no owner'
    $process = [Diagnostics.Process]::GetProcessById([int]$ownerPid)
    try {
      [void]$process.Handle
      [void]$process.StartTime
      [uint32]$confirmedPid = 0
      AWRequire ([GesherWin32]::GetWindowThreadProcessId($Window, [ref]$confirmedPid) -ne 0 -and
        $confirmedPid -eq $ownerPid -and -not $process.HasExited) 'Native HWND owner changed during capture'
    } catch {
      $process.Dispose()
      throw
    }
    [void]$owned.Add($process)
    [void]$windows.Add($Window)
    return $process
  }
  function AWWindow([string] $Role, [int] $Mark, [bool] $Separate) {
    $window = W3Window $read $Mark "Gesher app tasks $Role" 90
    AWRequire ($window -ne [IntPtr]::Zero) "No native first frame for $Role"
    $metadata = W3Wait $read $Mark "WINDOW_TASK role=$Role pid=(\d+) uid=(\d+) task=(\d+) separate=(true|false)" 30
    AWRequire $metadata.Success "No actual process/task metadata for $Role"
    $process = AWRetain $window
    AWRequire ($process.Id -eq [int]$metadata.Groups[1].Value) "HWND/process identity mismatch for $Role"
    AWRequire (($metadata.Groups[4].Value -eq 'true') -eq $Separate) "Wrong admitted process window mode for $Role"
    $entry = [pscustomobject]@{Role=$Role; Window=$window; Process=$process; Generation=$process.StartTime.ToUniversalTime().Ticks;
      Uid=[int]$metadata.Groups[2].Value; Task=[int]$metadata.Groups[3].Value}
    Write-Host ("APP_WINDOW_OWNER role={0} pid={1} generation={2} uid={3} task={4} hwnd={5:x} separate={6}" -f
      $Role, $process.Id, $entry.Generation, $entry.Uid, $entry.Task, $window.ToInt64(), $Separate)
    return $entry
  }
  function AWClick($Entry, [string] $Button) {
    $pattern = "WINDOW_BUTTONS role=$($Entry.Role) pid=$($Entry.Process.Id) [^\r\n]*?$Button=(\d+),(\d+)"
    AWRequire ((W3Wait $read 0 $pattern 30).Success) "No actual button layout for $($Entry.Role).$Button"
    $matches = [regex]::Matches("$(& $read)", $pattern)
    $point = $matches[$matches.Count - 1]
    [GesherWin32]::Click($Entry.Window, [int]$point.Groups[1].Value, [int]$point.Groups[2].Value)
  }
  function AWMenu($Entry, [string[]] $Roles, [string] $Selected) {
    $items = @()
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
      $items = @([GesherAppTaskMenu]::Read($Entry.Window))
      $labels = @($items | ForEach-Object { $_.Label })
      $complete = $items.Count -eq $Roles.Count + 1
      foreach ($role in $Roles) { $complete = $complete -and ($labels -contains "Gesher app tasks $role") }
      if ($complete) { break }
      Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    AWRequire $complete "Native system menu did not contain exactly the owned package tasks: $($labels -join '|')"
    $selectedItems = @($items | Where-Object { $_.Checked })
    AWRequire ($selectedItems.Count -eq 1 -and $selectedItems[0].Label -eq "Gesher app tasks $Selected") 'Native menu current-task check is wrong'
    return $items
  }
  function AWChoose($Entry, [string] $Role, [string[]] $Roles) {
    $items = @(AWMenu $Entry $Roles $Entry.Role)
    $item = @($items | Where-Object { $_.Label -eq "Gesher app tasks $Role" })
    AWRequire ($item.Count -eq 1) "Native menu target is ambiguous: $Role"
    AWRequire ([GesherWin32]::PostMessageW($Entry.Window, 0x0112, [IntPtr]$item[0].Id, [IntPtr]::Zero)) 'Native menu command was not posted'
  }
  function AWGone($Entry) {
    AWRequire (AWWait { -not [GesherWin32]::IsWindow($Entry.Window) } 60) "Closed HWND did not retire: $($Entry.Role)"
    AWRequire ($Entry.Process.WaitForExit(120000)) "Owned package process did not retire: $($Entry.Role)"
    AWRequire ($Entry.Process.ExitCode -eq 0) "Owned process exited abnormally: $($Entry.Role) $($Entry.Process.ExitCode)"
  }

  $succeeded = $false
  try {
    AWRequire ((Test-Path "$Dir/winapp.apk") -and (Test-Path "$Dir/winlegacy.apk")) 'Required actual APK suppliers are missing'
    AWRun @('launch', (NoDrive "$Dir/winlegacy.apk"), '--window', '--detach')
    $foreignWindow = W3Window $foreignRead 0 'Gesher windows4 legacy' 90
    AWRequire ($foreignWindow -ne [IntPtr]::Zero) 'Foreign-package control has no actual HWND'
    $foreignProcess = AWRetain $foreignWindow

    AWRun @('launch', (NoDrive "$Dir/winapp.apk"), '--window', '--activity', 'com.gesher.winapp.WindowTasksActivity', '--detach')
    $first = AWWindow WindowTasksActivity 0 $false
    $stateFile = Join-Path $profile 'system/gesher-device'
    $state = Get-Content -Raw -LiteralPath $stateFile
    $serverMatch = [regex]::Match($state, '(?m)^system_server=(\d+)\r?$')
    $generationMatch = [regex]::Match($state, '(?m)^system_server\.start=(\d+)\r?$')
    AWRequire ($serverMatch.Success -and $generationMatch.Success) 'No real device server identity'
    $server = [Diagnostics.Process]::GetProcessById([int]$serverMatch.Groups[1].Value)
    try {
      [void]$server.Handle
      $serverGeneration = $server.StartTime.ToUniversalTime().ToFileTimeUtc()
      AWRequire ($serverGeneration -eq [int64]$generationMatch.Groups[1].Value -and
        -not $server.HasExited) 'Device state/server generation mismatch'
    } catch {
      $server.Dispose()
      throw
    }
    [void]$owned.Add($server)

    # The saved next-start preference changes while the first process stays alive.
    AWRun @('windows', 'com.gesher.winapp', 'separate')
    AWRequire (Select-String -LiteralPath (Join-Path $profile 'system/app-windows.properties') -SimpleMatch 'separate.com.gesher.winapp=true' -Quiet) 'The real preference was not saved'
    $mark = W3Mark $read
    AWClick $first first
    $remote = AWWindow RemoteWindowActivity $mark $false
    AWRequire ($remote.Process.Id -ne $first.Process.Id -and $remote.Uid -eq $first.Uid -and $remote.Task -ne $first.Task) 'Remote task is not a distinct process/task of the same actual UID'
    AWRequire (AWWait { -not [GesherWin32]::IsWindowVisible($first.Window) -and [GesherWin32]::IsWindowVisible($remote.Window) }) 'A later process did not share the still-live package window'
    AWRequire (-not $first.Process.HasExited) 'The first process died instead of being covered'
    AWRequire ((W3Wait $read $mark 'WindowTasksActivity onStop' 30).Success) 'Covered process did not stop its Activity'
    AWChoose $remote WindowTasksActivity @('WindowTasksActivity', 'RemoteWindowActivity')
    AWRequire (AWWait { [GesherWin32]::IsWindowVisible($first.Window) -and -not [GesherWin32]::IsWindowVisible($remote.Window) }) 'Actual native menu did not switch the owned package tasks'
    Write-Host 'PASS  app windows: saved next-start mode retains both live process modes and native shared task/menu behavior'

    $mark = W3Mark $read
    AWClick $first second
    $minimized = AWWindow SecondRemoteWindowActivity $mark $false
    AWRequire ($minimized.Process.Id -ne $first.Process.Id -and $minimized.Process.Id -ne $remote.Process.Id -and $minimized.Uid -eq $first.Uid) 'Second remote task has no distinct same-UID process'
    [void][GesherW3Win32]::ShowWindowAsync($minimized.Window, 6)
    AWRequire (AWWait { [GesherW3Win32]::IsIconic($minimized.Window) -and [GesherWin32]::IsWindowVisible($minimized.Window) }) 'The actual minimized task lost its taskbar window'
    AWRequire ((W3Wait $read $mark 'SecondRemoteWindowActivity onStop' 30).Success) 'Minimized remote Activity did not stop'
    AWChoose $minimized WindowTasksActivity @('WindowTasksActivity', 'RemoteWindowActivity', 'SecondRemoteWindowActivity')
    AWRequire (AWWait { [GesherWin32]::IsWindowVisible($first.Window) -and [GesherW3Win32]::IsIconic($minimized.Window) }) 'Restoring a sibling did not preserve the separately minimized task'
    [void][GesherWin32]::PostMessageW($first.Window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    AWGone $first
    AWGone $remote
    AWRequire (-not $minimized.Process.HasExited -and [GesherW3Win32]::IsIconic($minimized.Window) -and [GesherWin32]::IsWindowVisible($minimized.Window)) 'X-close destroyed the user-minimized task in another process'
    AWRequire (-not $foreignProcess.HasExited -and [GesherWin32]::IsWindow($foreignWindow)) 'X-close crossed the package boundary'
    Write-Host 'PASS  app windows: X retires covered own tasks and their HWND/processes while preserving a genuinely minimized other-process task and a foreign app'

    # The one remaining minimized owner still holds the package setting.
    $mark = W3Mark $read
    AWRun @('launch', 'com.gesher.winapp', '--window', '--activity', 'com.gesher.winapp.WindowTasksActivity', '--detach')
    $remaining = AWWindow WindowTasksActivity $mark $false
    AWRequire ($remaining.Process.Id -ne $minimized.Process.Id -and $remaining.Uid -eq $minimized.Uid) 'Reopened main task is not a same-UID additional process'
    [void][GesherWin32]::PostMessageW($remaining.Window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    AWGone $remaining
    AWRequire (-not $minimized.Process.HasExited) 'Last minimized owner did not survive a second X-close'
    $mark = W3Mark $read
    [void][GesherW3Win32]::ShowWindowAsync($minimized.Window, 9)
    AWRequire (AWWait { -not [GesherW3Win32]::IsIconic($minimized.Window) -and [GesherWin32]::IsWindowVisible($minimized.Window) }) 'Minimized surviving task did not restore'
    # The restored window takes input once its Activity resumes; a stopped window drops it, as on Android.
    AWRequire ((W3Wait $read $mark 'SecondRemoteWindowActivity onTopResumedActivityChanged true' 30).Success) 'Restored minimized task did not resume'
    AWClick $minimized finish
    AWGone $minimized

    # All package processes are gone, while the same real system server is still alive.
    AWRequire (-not $server.HasExited -and $server.StartTime.ToUniversalTime().ToFileTimeUtc() -eq $serverGeneration) 'The server restarted before the last-owner setting check'
    AWRequire (-not $foreignProcess.HasExited) 'Unrelated live-package control unexpectedly exited'
    $mark = W3Mark $read
    AWRun @('launch', 'com.gesher.winapp', '--window', '--activity', 'com.gesher.winapp.WindowTasksActivity', '--detach')
    $next = AWWindow WindowTasksActivity $mark $true
    $mark = W3Mark $read
    AWClick $next first
    $nextRemote = AWWindow RemoteWindowActivity $mark $true
    AWRequire ($nextRemote.Process.Id -ne $next.Process.Id -and $nextRemote.Uid -eq $next.Uid) 'Next lifetime did not create an actual additional same-UID process'
    AWRequire (AWWait { [GesherWin32]::IsWindowVisible($next.Window) -and [GesherWin32]::IsWindowVisible($nextRemote.Window) }) 'The next lifetime did not apply the saved separate-window choice'
    AWRequire (@([GesherAppTaskMenu]::Read($nextRemote.Window)).Count -eq 0) 'Separate mode retained the shared app task menu'
    [void][GesherWin32]::PostMessageW($next.Window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    AWGone $next
    AWRequire (-not $nextRemote.Process.HasExited -and [GesherWin32]::IsWindowVisible($nextRemote.Window)) 'Separate-mode X closed another task'
    AWClick $nextRemote finish
    AWGone $nextRemote
    Write-Host 'PASS  app windows: mode lasts through the final minimized owner and changes only after all package processes retire, with the same system server'
    $succeeded = $true
  } catch {
    Write-Host "FAIL  app windows: $_ $($_.ScriptStackTrace)"
    $script:failed++
  } finally {
    foreach ($window in $windows) {
      if ([GesherWin32]::IsWindow($window)) { [void][GesherWin32]::PostMessageW($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
    }
    try { AWRun @('shutdown') }
    catch { Write-Host "FAIL  app windows: owned device shutdown: $_"; $script:failed++ }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    foreach ($process in $owned) {
      $handle = $null
      try {
        $handle = $process.SafeHandle
        $left = [Math]::Max(0, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        if (-not $process.HasExited -and -not $process.WaitForExit($left)) {
          Write-Host "FAIL  app windows: owned PID $($process.Id) remained after shutdown"
          $script:failed++
          # The retained process handle, never a newly opened/reused PID.
          $process.Kill()
          AWRequire ($process.WaitForExit(10000)) 'Owned process did not retire after its forced cleanup'
        }
      } catch { Write-Host "FAIL  app windows: handle cleanup: $_"; $script:failed++ }
      finally {
        try {
          $process.Dispose()
          if ($null -ne $handle -and -not $handle.IsClosed) { throw 'Owned process handle was not closed' }
        } catch { Write-Host "FAIL  app windows: handle disposal: $_"; $script:failed++ }
      }
    }
    foreach ($window in $windows) {
      if ([GesherWin32]::IsWindow($window)) { Write-Host "FAIL  app windows: owned HWND $window survived retirement"; $script:failed++ }
    }
    foreach ($name in $saved.Keys) {
      [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process')
    }
    if (-not $succeeded) {
      foreach ($file in Get-ChildItem -LiteralPath $logs -Filter '*.log' -ErrorAction SilentlyContinue) {
        Write-Host "APP_WINDOW_LOG $($file.Name)"
        Get-Content -LiteralPath $file.FullName -Tail 180 | Out-Host
      }
    }
  }
}
Test-AppWindowLifetime
