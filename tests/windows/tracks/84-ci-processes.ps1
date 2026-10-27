# Compile and execute only on CI Windows hosts; use the actual shared helper.
$process84Root = Join-Path $root ('process84-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $process84Root | Out-Null
$process84Exe = Join-Path $process84Root 'process84.exe'
$process84Compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$process84Source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../fixtures/Process84.cs')).Path
Write-Host "INFO  CI processes: compiler $process84Compiler source $process84Source output $process84Exe"
& $process84Compiler /nologo /warnaserror+ /platform:x64 /target:exe "/out:$process84Exe" $process84Source
if ($LASTEXITCODE -ne 0) {
  Write-Host 'FAIL  CI processes: actual process fixture compilation failed'
  $script:failed++
  return
}

# Exercise the actual legacy helpers, preserving their original 120 s exit
# and SurfaceRun's 8 s diagnostic/500 s phase bounds. Phases=0 isolates exit
# cleanup without fabricating HWNDs, pixels or app phase markers.
$process84SavedDir = $Dir
$process84SavedSlHost = $slHost
$process84SavedSurfaceVm = $dvmSurfaces
$process84SavedWindowVm = $dvmWindows
$process84SavedTrace = $env:GESHER_CI_PROCESS_TRACE
$process84SurfaceDir = Join-Path $process84Root 'surface-helper'
New-Item -ItemType Directory $process84SurfaceDir | Out-Null
$process84SurfaceExe = Join-Path $process84SurfaceDir 'dalvikvm.exe'
Copy-Item -LiteralPath $process84Exe -Destination $process84SurfaceExe
try {
  $Dir = $process84SurfaceDir
  $slHost = $process84SurfaceDir
  $dvmSurfaces = @()
  $dvmWindows = @()
  $env:GESHER_CI_PROCESS_TRACE = '1'
  foreach ($helper in 'SurfaceRun', 'InvokeOpenSl') {
    foreach ($case in @(@{ Mode = 'normal'; Expected = 0 }, @{ Mode = 'nonzero'; Expected = 23 },
                       @{ Mode = 'timeout'; Expected = -1 }, @{ Mode = 'retained'; Expected = -1 })) {
      $pidFile = Join-Path $process84Root "$helper-$($case.Mode).pid"
      $ownedExe = $process84SurfaceExe
      $expected = if ($helper -eq 'SurfaceRun' -and $case.Mode -eq 'retained') { 0 } else { $case.Expected }
      $timer = [Diagnostics.Stopwatch]::StartNew()
      try {
        if ($helper -eq 'SurfaceRun') {
          $result = SurfaceRun -Arguments @($case.Mode, $pidFile) -Tag "process84-$($case.Mode)" -AppLogs @() -Phases 0
          $code = $result.Exit
        } else {
          $result = InvokeOpenSl -Arguments @($case.Mode, $pidFile)
          $code = $result.Code
        }
        $text = $result.Text
        $timer.Stop()
        $bound = if ($case.Mode -eq 'timeout') { 145 } else { 25 }
        $correct = $code -eq $expected -and $timer.Elapsed.TotalSeconds -lt $bound
        if ($case.Mode -in @('normal', 'nonzero')) {
          $correct = $correct -and $text.Contains("process84 stdout $($case.Mode)") -and
            $text.Contains("process84 stderr $($case.Mode)")
        } elseif ($case.Mode -eq 'timeout') {
          $marker = if ($helper -eq 'SurfaceRun') { '(killed: still running 120 s after its last phase)' } else { '(timed out after 120 s)' }
          $correct = $correct -and $text.Contains($marker) -and (Test-Path -LiteralPath $pidFile)
        } else {
          # SurfaceRun redirects to a file, not an EOF-waiting pipe: the
          # actual parent may finish while a child still holds that file.
          $retained = if (Test-Path -LiteralPath $pidFile) {
            Get-Process -Id ([int](Get-Content -LiteralPath $pidFile -Raw)) -ErrorAction SilentlyContinue
          } else { $null }
          try { $correct = $correct -and $null -ne $retained -and -not $retained.HasExited }
          finally { if ($retained) { $retained.Dispose() } }
          if ($helper -eq 'InvokeOpenSl') { $correct = $correct -and $text.Contains('did not complete within five seconds') }
        }
        if ($correct) {
          Write-Host "PASS  CI processes: actual $helper $($case.Mode) status$code output/retained-handle evidence within original bounds"
        } else {
          Write-Host "FAIL  CI processes: actual $helper $($case.Mode) status$code expected$expected after$($timer.Elapsed.TotalSeconds): $text"
          $script:failed++
        }
      } finally {
        if (Test-Path -LiteralPath $pidFile) {
          $ownedPid = [int](Get-Content -LiteralPath $pidFile -Raw)
          $ownedProcess = Get-Process -Id $ownedPid -ErrorAction SilentlyContinue
          if ($ownedProcess) {
            try {
              if ($ownedProcess.Path -ne $ownedExe) { throw 'legacy-helper recorded PID is not its owned fixture' }
              $ownedProcess.Kill($true)
              if (-not $ownedProcess.WaitForExit(5000)) { throw 'legacy-helper fixture survived bounded cleanup' }
            } catch {
              Write-Host "FAIL  CI processes: actual $helper $($case.Mode) fixture cleanup: $_"
              $script:failed++
            } finally { $ownedProcess.Dispose() }
          }
        }
      }
    }
  }
  foreach ($helper in 'WaitTimed', 'LocaleWait') {
    foreach ($case in @(@{ Mode = 'normal'; Expected = 0 }, @{ Mode = 'nonzero'; Expected = 23 },
                       @{ Mode = 'timeout'; Expected = -1 }, @{ Mode = 'retained'; Expected = -1 })) {
      $pidFile = Join-Path $process84Root "$helper-$($case.Mode).pid"
      $limit = if ($case.Mode -eq 'timeout') { 3 } else { 15 }
      $timer = [Diagnostics.Stopwatch]::StartNew()
      try {
        $session = if ($helper -eq 'WaitTimed') {
          StartTimed $process84SurfaceExe @($case.Mode, $pidFile)
        } else { LocaleStart @($case.Mode, $pidFile) }
        $actualHandle = $session.Process.SafeHandle
        if ($case.Mode -eq 'retained' -and -not $session.Process.WaitForExit(5000)) {
          throw 'The actual retained-pipe parent did not exit before the split wait'
        }
        $text = & $helper $session $limit
        $code = $LASTEXITCODE
        $timer.Stop()
        $correct = $code -eq $case.Expected -and $timer.Elapsed.TotalSeconds -lt ($limit + 25) -and $actualHandle.IsClosed
        if ($case.Mode -in @('normal', 'nonzero')) {
          $correct = $correct -and $text.Contains("process84 stdout $($case.Mode)") -and
            $text.Contains("process84 stderr $($case.Mode)")
        } elseif ($case.Mode -eq 'timeout') {
          $correct = $correct -and $text.Contains('(timed out after 3 s)') -and (Test-Path -LiteralPath $pidFile)
        } else {
          $correct = $correct -and $text.Contains('did not complete within ten seconds') -and (Test-Path -LiteralPath $pidFile)
        }
        if ($correct) {
          Write-Host "PASS  CI processes: actual split $helper $($case.Mode) status$code bounded output and disposed native handle"
        } else {
          Write-Host "FAIL  CI processes: actual split $helper $($case.Mode) status$code expected$($case.Expected) disposed$($actualHandle.IsClosed) after$($timer.Elapsed.TotalSeconds): $text"
          $script:failed++
        }
      } finally {
        if (Test-Path -LiteralPath $pidFile) {
          $ownedProcess = Get-Process -Id ([int](Get-Content -LiteralPath $pidFile -Raw)) -ErrorAction SilentlyContinue
          if ($ownedProcess) {
            try {
              if ($ownedProcess.Path -ne $process84SurfaceExe) { throw 'split helper PID is not the owned fixture' }
              $ownedProcess.Kill($true)
              if (-not $ownedProcess.WaitForExit(5000)) { throw 'split helper fixture survived bounded cleanup' }
            } catch {
              Write-Host "FAIL  CI processes: actual split $helper $($case.Mode) cleanup: $_"
              $script:failed++
            } finally { $ownedProcess.Dispose() }
          }
        }
      }
    }
  }
  $process84N2Work = Join-Path $process84Root 'native2-cwd'
  New-Item -ItemType Directory $process84N2Work | Out-Null
  $process84N2Value = 'process84-' + [guid]::NewGuid().ToString('N')
  foreach ($case in @(@{ Mode = 'normal'; Expected = 0 }, @{ Mode = 'nonzero'; Expected = 23 },
                     @{ Mode = 'timeout'; Expected = 124 }, @{ Mode = 'retained'; Expected = 124 },
                     @{ Mode = 'environment'; Expected = 0 })) {
    $pidFile = Join-Path $process84Root "N2Run-$($case.Mode).pid"
    $limit = if ($case.Mode -eq 'timeout') { 3 } else { 15 }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
      $text = N2Run $process84Exe @($case.Mode, $pidFile) $process84N2Work @{ GESHER_PROCESS84_ENV = $process84N2Value } $limit | Out-String
      $code = $LASTEXITCODE
      $timer.Stop()
      $correct = $code -eq $case.Expected -and $timer.Elapsed.TotalSeconds -lt ($limit + 25) -and
        $text -match 'INFO N2 process cleanup: pid=\d+ native handle closed=True'
      if ($case.Mode -in @('normal', 'nonzero', 'environment')) {
        $correct = $correct -and $text.Contains("process84 stdout $($case.Mode)") -and
          $text.Contains("process84 stderr $($case.Mode)")
        if ($case.Mode -eq 'environment') {
          $correct = $correct -and $text.Contains("process84 cwd $process84N2Work") -and
            $text.Contains("process84 environment $process84N2Value")
        }
      } elseif ($case.Mode -eq 'timeout') {
        $correct = $correct -and $text.Contains('TIMEOUT after 3 s:') -and (Test-Path -LiteralPath $pidFile)
      } else {
        $correct = $correct -and $text.Contains('did not complete within ten seconds') -and (Test-Path -LiteralPath $pidFile)
      }
      if ($correct) {
        Write-Host "PASS  CI processes: actual N2Run $($case.Mode) status$code workdir/environment/stream output and disposed native handle"
      } else {
        Write-Host "FAIL  CI processes: actual N2Run $($case.Mode) status$code expected$($case.Expected) after$($timer.Elapsed.TotalSeconds): $text"
        $script:failed++
      }
    } finally {
      if (Test-Path -LiteralPath $pidFile) {
        $ownedProcess = Get-Process -Id ([int](Get-Content -LiteralPath $pidFile -Raw)) -ErrorAction SilentlyContinue
        if ($ownedProcess) {
          try {
            if ($ownedProcess.Path -ne $process84Exe) { throw 'N2Run PID is not the owned fixture' }
            $ownedProcess.Kill($true)
            if (-not $ownedProcess.WaitForExit(5000)) { throw 'N2Run fixture survived bounded cleanup' }
          } catch {
            Write-Host "FAIL  CI processes: actual N2Run $($case.Mode) cleanup: $_"
            $script:failed++
          } finally { $ownedProcess.Dispose() }
        }
      }
    }
  }
} finally {
  $Dir = $process84SavedDir
  $slHost = $process84SavedSlHost
  $dvmSurfaces = $process84SavedSurfaceVm
  $dvmWindows = $process84SavedWindowVm
  $env:GESHER_CI_PROCESS_TRACE = $process84SavedTrace
}
foreach ($case in @(@{ Mode = 'normal'; Expected = 0 }, @{ Mode = 'nonzero'; Expected = 23 },
                   @{ Mode = 'timeout'; Expected = -1 }, @{ Mode = 'retained'; Expected = -1 })) {
  $pidFile = Join-Path $process84Root "$($case.Mode).pid"
  $limit = if ($case.Mode -eq 'timeout') { 3 } else { 15 }
  $timer = [Diagnostics.Stopwatch]::StartNew()
  try {
    $text = RunTimed $process84Exe @($case.Mode, $pidFile) $limit
    $code = $LASTEXITCODE
    $timer.Stop()
    $correct = $code -eq $case.Expected -and $timer.Elapsed.TotalSeconds -lt ($limit + 20)
    if ($case.Mode -in @('normal', 'nonzero')) {
      $correct = $correct -and $text.Contains("process84 stdout $($case.Mode)") -and
        $text.Contains("process84 stderr $($case.Mode)")
    } elseif ($case.Mode -eq 'timeout') {
      $correct = $correct -and $text.Contains('(timed out after 3 s)') -and (Test-Path $pidFile)
    } else {
      $correct = $correct -and $text.Contains('did not complete within five seconds') -and (Test-Path $pidFile)
    }
    if ($correct) {
      Write-Host "PASS  CI processes: $($case.Mode) preserves status/output and completes within explicit bounds"
    } else {
      Write-Host "FAIL  CI processes: $($case.Mode) code$code expected$($case.Expected) after$($timer.Elapsed.TotalSeconds): $text"
      $script:failed++
    }
  } finally {
    if (Test-Path $pidFile) {
      $ownedPid = [int] (Get-Content -LiteralPath $pidFile -Raw)
      $ownedProcess = Get-Process -Id $ownedPid -ErrorAction SilentlyContinue
      if ($ownedProcess) {
        try {
          if ($ownedProcess.Path -ne $process84Exe) { throw 'recorded process does not match the owned fixture' }
          $ownedProcess.Kill($true)
          if (-not $ownedProcess.WaitForExit(5000)) { throw 'owned fixture cleanup exceeded five seconds' }
        } catch {
          Write-Host "FAIL  CI processes: $($case.Mode) owned fixture cleanup: $_"
          $script:failed++
        } finally { $ownedProcess.Dispose() }
      }
    }
  }
}
