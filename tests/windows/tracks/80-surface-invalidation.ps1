# SurfaceView invalidation liveness (surfaces2 ADR0063; combined Media4 candidate).
# All native/C#/JVM compilation and execution happen only on the CI runners.
function InvalidationBounded([string] $Exe, [string[]] $Arguments, [int] $Seconds) {
  $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.CreateNoWindow = $true
  foreach ($argument in $Arguments) { $psi.ArgumentList.Add($argument) }
  $process = [Diagnostics.Process]::Start($psi)
  $text = ''
  $timedOut = $false
  try {
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($Seconds * 1000)) {
      $timedOut = $true
      $text += "process exceeded $Seconds seconds`n"
      try { $process.Kill($true) } catch { $text += "process kill failed: $_`n" }
      if (-not $process.WaitForExit(5000)) { $text += "process still alive five seconds after kill`n" }
    }
    foreach ($pipe in @(@{ Task = $stdout; Name = 'stdout' }, @{ Task = $stderr; Name = 'stderr' })) {
      try {
        if ($pipe.Task.Wait(5000)) { $text += $pipe.Task.Result }
        else { $timedOut = $true; $text += "$($pipe.Name) did not finish within five seconds`n" }
      } catch { $timedOut = $true; $text += "$($pipe.Name) read failed: $_`n" }
    }
    return @{ Text = $text; Exit = $(if ($timedOut) { 124 } else { $process.ExitCode }) }
  } finally { $process.Dispose() }
}

$ivControls = @('no-env-then-recovery', 'no-view-then-recovery',
  'java-post-failure-then-recovery', 'coalesce-until-draw',
  'bind-before-acknowledge', 'concurrent-producers-coalesce', 'old-failure-keeps-new-post')
$ivRun = InvalidationBounded "$Dir/surface_invalidation_test.exe" @() 30
$ivText = $ivRun.Text
$ivExit = $ivRun.Exit
$ivMissing = @($ivControls | Where-Object { -not $ivText.Contains("surface invalidation ${_}: ok") })
if ($ivExit -eq 0 -and $ivMissing.Count -eq 0 -and $ivText.Contains('surface invalidation: 0 failure(s)')) {
  Write-Host 'PASS  surface invalidation: failed posts recover, newer posts survive old failures, successful posts coalesce, binding precedes acknowledgement under controlled barriers'
} else {
  Write-Host "FAIL  surface invalidation: production scheduling controls (exit $ivExit; missing $($ivMissing -join ', '))"
  Write-Host $ivText
  $script:failed++
}
foreach ($line in @($ivText -split "`r?`n" | Where-Object { $_ })) { Write-Host "INFO  $line" }

# Reuse the existing real SurfaceView/EGL capture helper. It sends no input or
# explicit invalidate to this activity and retains the original color thresholds.
function InvalidationPhaseChecks([string] $Mode, [string] $Abi, $Result) {
  $problems = @()
  if (-not $Result.Text.Contains("surfacegl: invalidation: abi $Abi")) { $problems += "missing actual native ABI $Abi" }
  if (-not $Result.Text.Contains('surfacegl: invalidation: legacy variadic probe returned')) {
    $problems += 'the original printf/vprintf/fflush literal/integer cold path did not return'
  }
  foreach ($phase in 1, 2) {
    if ($null -eq $Result.Shots[$phase]) { $problems += "no native phase $phase capture" }
    if (-not $Result.Text.Contains("surfacegl: invalidation: phase $phase ready")) {
      $problems += "no native producer phase $phase"
    }
  }
  if ($Result.Shots[1]) {
    $problems += @(Regions $Result.Shots[1] @(
      @('initial red', 10, 10, 190, 390, 0xff0000), @('initial green', 210, 10, 390, 390, 0x00ff00)))
  }
  if ($Result.Shots[2]) {
    $problems += @(Regions $Result.Shots[2] @(
      @('producer blue', 10, 10, 190, 390, 0x0000ff), @('producer yellow', 210, 10, 390, 390, 0xffff00)))
  }
  if ($Result.Exit -ne 0) { $problems += "process exit $($Result.Exit)" }
  if ($Mode -like 'device *') {
    $ended = [regex]::Match($Result.Text, '(?m)^gesher-app: the process of com\.gesher\.surfacegl ended \(exit code (-?\d+)\)\r?$')
    if (-not $ended.Success -or $ended.Groups[1].Value -cne '0') { $problems += 'missing normal exit-code-zero completion of the actual app process' }
    Write-Host "INFO  surface invalidation ($Mode) actual child: $(if ($ended.Success) { $ended.Value.Trim() } else { '(missing)' })"
  }
  if (-not $Result.Text.Contains('surfacegl: invalidation: surface destroyed') -or
      -not $Result.Text.Contains('surfacegl: invalidation: done')) { $problems += 'missing complete teardown' }
  $problems = @($problems | Where-Object { $_ })
  if ($problems.Count -eq 0) {
    Write-Host "PASS  surface invalidation ($Mode): producer-only red/green to blue/yellow frames reach PrintWindow without input, resize or View.invalidate"
  } else {
    Write-Host "FAIL  surface invalidation ($Mode): $($problems -join '; ')"
    Write-Host $Result.Text
    $script:failed++
  }
  foreach ($line in @($Result.Text -split "`r?`n" | Where-Object {
      $_ -match 'surfacegl: invalidation:|GesherSurface invalidation' } | Select-Object -Unique)) {
    Write-Host "INFO  surface invalidation ($Mode): $line"
  }
}

$ivSaved = @{}
foreach ($key in @('ANDROID_DATA', 'GESHER_LOGS', 'GESHER_DISPLAY', 'GESHER_SURFACE_TRACE_INVALIDATION')) {
  $ivSaved[$key] = [Environment]::GetEnvironmentVariable($key)
}
try {
  $env:GESHER_SURFACE_TRACE_INVALIDATION = '1'
  $ivActivity = 'com.gesher.surfacegl.InvalidationActivity'
  foreach ($ivAbi in @('x86_64', 'arm64-v8a')) {
    $ivFile = if ($ivAbi -eq 'x86_64') { 'surfacegl.apk' } else { 'surfacegl-arm64.apk' }
    $ivApk = NoDrive "$Dir/$ivFile"
    $env:ANDROID_DATA = $ivSaved['ANDROID_DATA']
    $env:GESHER_LOGS = $ivSaved['GESHER_LOGS']
    $env:GESHER_DISPLAY = $ivSaved['GESHER_DISPLAY']
    $ivResult = SurfaceRun @('com.gesher.app.Main', '--hidden-window', '--display', '400x800@160',
      '--activity', $ivActivity, $ivApk) "invalidation-single-$ivAbi" @() 'surfacegl: invalidation' 2
    InvalidationPhaseChecks "single process $ivAbi" $ivAbi $ivResult

    $ivRoot = Join-Path $root "invalidation-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Force "$ivRoot/data", "$ivRoot/logs" | Out-Null
    $env:ANDROID_DATA = NoDrive "$ivRoot/data"
    $env:GESHER_LOGS = NoDrive "$ivRoot/logs"
    $env:GESHER_DISPLAY = '400x800@160'
    $watchStarted = [DateTime]::UtcNow
    $watch = Start-ThreadJob -ArgumentList @("$ivRoot/logs/com.gesher.surfacegl.log", (Resolve-Path "$Dir/dalvikvm.exe").Path, $watchStarted) -ScriptBlock {
      param($log, $expectedExe, $started)
      $until = [DateTime]::UtcNow.AddSeconds(620)
      while ([DateTime]::UtcNow -lt $until) {
        $text = Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue
        $match = [regex]::Match("$text", 'app process for com\.gesher\.surfacegl: pid (\d+)')
        if ($match.Success) {
          $child = Get-Process -Id ([int]$match.Groups[1].Value) -ErrorAction SilentlyContinue
          if ($child) {
            try {
              $handle = $child.Handle # Pin this actual process, even if its PID is later reused.
              $born = $child.StartTime.ToUniversalTime()
              if ($born -lt $started -or $child.Path -ine $expectedExe) { throw 'Unexpected app process birth or executable' }
              if (-not $child.WaitForExit(620000)) { throw 'Actual app process did not exit within observer bound' }
              return @{ Pid = $child.Id; Birth = $born; Exit = $child.ExitCode; End = $child.ExitTime.ToUniversalTime(); Handle = "$handle" }
            } finally { $child.Dispose() }
          }
        }
        Start-Sleep -Milliseconds 100
      }
      throw 'Did not acquire the actual fresh app process while it was running'
    }
    try {
      $ivResult = SurfaceRun @('com.gesher.app.Launcher', 'launch', $ivApk, '--hidden-window',
        '--activity', $ivActivity) "invalidation-device-$ivAbi" @("$ivRoot/logs/com.gesher.surfacegl.log") 'surfacegl: invalidation' 2
      $observedDeadline = [DateTime]::UtcNow
      $null = Wait-Job -Job $watch -Timeout 2
      $childEvidence = if ($watch.State -eq 'Completed') { Receive-Job -Job $watch } else { $null }
      if (-not $childEvidence -or $childEvidence.Exit -ne 0 -or $childEvidence.End -gt $observedDeadline) {
        Write-Host "FAIL  surface invalidation ($ivAbi): fresh actual app process did not independently complete normally within the original run"
        $script:failed++
      } else {
        Write-Host "INFO  surface invalidation ($ivAbi): owned app pid=$($childEvidence.Pid) birth=$($childEvidence.Birth.ToString('o')) native-exit=$($childEvidence.Exit) ended=$($childEvidence.End.ToString('o'))"
      }
      InvalidationPhaseChecks "device $ivAbi" $ivAbi $ivResult
    } finally {
      if ($watch.State -in @('NotStarted','Running')) { Stop-Job -Job $watch }
      Remove-Job -Job $watch -Force
      $ivStop = InvalidationBounded "$Dir/dalvikvm.exe" (@($dvmSurfaces) + @('com.gesher.app.Launcher', 'shutdown')) 30
      if ($ivStop.Exit -ne 0) {
        Write-Host "FAIL  surface invalidation ($ivAbi): device shutdown exit $($ivStop.Exit): $($ivStop.Text)"
        $script:failed++
      } else { Write-Host "PASS  surface invalidation ($ivAbi): device shutdown completes" }
    }
  }
} finally {
  foreach ($key in $ivSaved.Keys) { [Environment]::SetEnvironmentVariable($key, $ivSaved[$key]) }
}
