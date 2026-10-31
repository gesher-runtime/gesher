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

# Compile the gtest parser/retained-pipe fixture only on CI. RetainOutput is the
# same real Win32 handle-transfer routine exercised by the existing controls.
$gtest84Exe = Join-Path $process84Root 'gtest84.exe'
$gtest84Source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../fixtures/Gtest84.cs')).Path
& $process84Compiler /nologo /warnaserror+ /platform:x64 /target:exe /main:Gtest84 "/out:$gtest84Exe" $gtest84Source $process84Source
if ($LASTEXITCODE -ne 0) {
  Write-Host 'FAIL  CI processes: actual gtest fixture compilation failed'
  $script:failed++
  return
}
$gtest84Saved = @{}
foreach ($name in 'GESHER_GTEST84_MODE','GESHER_GTEST84_PID','GESHER_GTEST84_CWD','GESHER_CI_PROCESS_TRACE') {
  $gtest84Saved[$name] = [Environment]::GetEnvironmentVariable($name)
}
$gtest84SavedLibs = $libsTests
$gtest84SavedArt = $artBin
if (-not ('Gtest84NativeHandles' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Gtest84NativeHandles {
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool GetHandleInformation(IntPtr handle, out uint flags);
  public static bool Closed(IntPtr handle) {
    uint flags;
    return handle != IntPtr.Zero && !GetHandleInformation(handle, out flags) && Marshal.GetLastWin32Error() == 6;
  }
}
'@
}
Write-Host "ACTUAL_GTEST_RUNTIME powershell=$($PSVersionTable.PSVersion) framework=$([Runtime.InteropServices.RuntimeInformation]::FrameworkDescription)"
try {
  $env:GESHER_CI_PROCESS_TRACE = '1'
  foreach ($helper in 'Invoke-CoreLibsTest','Invoke-ArtGtest') {
    $directory = Join-Path $process84Root $helper
    New-Item -ItemType Directory $directory | Out-Null
    Copy-Item -LiteralPath $gtest84Exe -Destination (Join-Path $directory 'gtest84.exe')
    $libsTests = $directory
    $artBin = $directory
    $env:GESHER_GTEST84_CWD = $directory
    foreach ($mode in 'normal','normal-relative','nonzero','failed','zero-count','missing-summary','timeout','retained','missing-exe') {
      $env:GESHER_GTEST84_MODE = if ($mode -eq 'normal-relative') { 'normal' } else { $mode }
      $pidFile = Join-Path $process84Root "$helper-$mode.pid"
      $env:GESHER_GTEST84_PID = $pidFile
      $name = if ($mode -eq 'missing-exe') { 'absent84' } else { 'gtest84' }
      $timer = [Diagnostics.Stopwatch]::StartNew()
      $relative = $mode -eq 'normal-relative'
      try {
        if ($relative) {
          Push-Location $process84Root
          $libsTests = Split-Path -Leaf $directory
          $artBin = $libsTests
        }
        $records = @(& $helper -Name $name -Exclude @('Fixture.Excluded') -TimeoutSec 1 6>&1)
        $timer.Stop()
        $verdicts = @($records | Where-Object { $_ -is [bool] })
        $text = ($records | Where-Object { $_ -isnot [bool] } | ForEach-Object { "$_" }) -join "`n"
        $expected = $mode -in @('normal','normal-relative')
        $correct = $verdicts.Count -eq 1 -and $verdicts[0] -eq $expected -and $timer.Elapsed.TotalSeconds -lt 20
        if ($mode -ne 'missing-exe') {
          $pipeEvidence = [regex]::Match($text, 'tasksdone=True pipesclosed=True pipe-output=(\d+) pipe-error=(\d+)')
          $actualPipesClosed = $pipeEvidence.Success -and
            [Gtest84NativeHandles]::Closed([IntPtr][long]$pipeEvidence.Groups[1].Value) -and
            [Gtest84NativeHandles]::Closed([IntPtr][long]$pipeEvidence.Groups[2].Value)
          $correct = $correct -and $text.Contains('disposed=True') -and $actualPipesClosed
          Write-Host "ACTUAL_GTEST_PIPE_CLEANUP helper=$helper case=$mode tasks-stopped=$($pipeEvidence.Success) native-pipes-invalid=$actualPipesClosed"
        }
        if ($expected) { $correct = $correct -and $text.Contains('1 tests passed') -and $text.Contains('readers=True') }
        if ($mode -eq 'nonzero') { $correct = $correct -and $text.Contains('exit 0x00000017') }
        if ($mode -eq 'failed') { $correct = $correct -and $text.Contains('Fixture.RealProcess') -and $text.Contains('1 failed') }
        if ($mode -eq 'timeout') { $correct = $correct -and $text.Contains('timed out after 1 s') }
        if ($mode -eq 'retained') {
          $correct = $correct -and $text.Contains('complete=True readers=False') -and
            $text.Contains('output did not complete within ten seconds') -and (Test-Path -LiteralPath $pidFile)
          if (Test-Path -LiteralPath $pidFile) {
            $retained = Get-Process -Id ([int](Get-Content -LiteralPath $pidFile -Raw)) -ErrorAction SilentlyContinue
            try { $correct = $correct -and $null -ne $retained -and -not $retained.HasExited }
            finally { if ($retained) { $retained.Dispose() } }
          }
        }
        $text | Out-Host
        if ($correct) { Write-Host "PASS  CI processes: actual $helper $mode parser/status/output/disposal within original bounds" }
        else { Write-Host "FAIL  CI processes: actual $helper $mode verdict=$($verdicts -join ',') expected=$expected after$($timer.Elapsed.TotalSeconds)"; $script:failed++ }
      } finally {
        if ($relative) { Pop-Location; $libsTests = $directory; $artBin = $directory }
        if (Test-Path -LiteralPath $pidFile) {
          $owned = Get-Process -Id ([int](Get-Content -LiteralPath $pidFile -Raw)) -ErrorAction SilentlyContinue
          if ($owned) {
            try {
              if ($owned.Path -ne (Join-Path $directory 'gtest84.exe')) { throw 'gtest fixture child PID is not the owned executable' }
              $owned.Kill($true)
              if (-not $owned.WaitForExit(5000)) { throw 'gtest fixture child survived finite cleanup' }
            } catch { Write-Host "FAIL  CI processes: gtest fixture cleanup $_"; $script:failed++ }
            finally { $owned.Dispose() }
          }
        }
      }
    }
  }
} finally {
  $libsTests = $gtest84SavedLibs
  $artBin = $gtest84SavedArt
  foreach ($name in $gtest84Saved.Keys) { [Environment]::SetEnvironmentVariable($name, $gtest84Saved[$name]) }
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
        if ($helper -eq 'InvokeOpenSl') {
          $pipeEvidence = [regex]::Match($text, 'INFO READER_CLEANUP helper=InvokeOpenSl tasksdone=True pipesclosed=True processclosed=True pipe-output=(\d+) pipe-error=(\d+)')
          $invalid = $pipeEvidence.Success -and
            [Gtest84NativeHandles]::Closed([IntPtr][long]$pipeEvidence.Groups[1].Value) -and
            [Gtest84NativeHandles]::Closed([IntPtr][long]$pipeEvidence.Groups[2].Value)
          $correct = $correct -and $invalid
          Write-Host "ACTUAL_READER_PIPE_CLEANUP helper=InvokeOpenSl case=$($case.Mode) tasks-stopped=$($pipeEvidence.Success) native-pipes-invalid=$invalid"
        }
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
$runTimed84SavedTrace = $env:GESHER_CI_PROCESS_TRACE
try {
$env:GESHER_CI_PROCESS_TRACE = '1'
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
    $pipeEvidence = [regex]::Match($text, 'INFO READER_CLEANUP helper=RunTimed tasksdone=True pipesclosed=True processclosed=True pipe-output=(\d+) pipe-error=(\d+)')
    $invalid = $pipeEvidence.Success -and
      [Gtest84NativeHandles]::Closed([IntPtr][long]$pipeEvidence.Groups[1].Value) -and
      [Gtest84NativeHandles]::Closed([IntPtr][long]$pipeEvidence.Groups[2].Value)
    $correct = $correct -and $invalid
    Write-Host "ACTUAL_READER_PIPE_CLEANUP helper=RunTimed case=$($case.Mode) tasks-stopped=$($pipeEvidence.Success) native-pipes-invalid=$invalid"
    if ($case.Mode -in @('normal', 'nonzero')) {
      $correct = $correct -and $text.Contains("process84 stdout $($case.Mode)") -and
        $text.Contains("process84 stderr $($case.Mode)")
    } elseif ($case.Mode -eq 'timeout') {
      $correct = $correct -and $text.Contains('(timed out after 3 s)') -and (Test-Path $pidFile)
    } else {
      $correct = $correct -and $text.Contains('did not complete within five seconds') -and (Test-Path $pidFile)
      if (Test-Path -LiteralPath $pidFile) {
        $writer = Get-Process -Id ([int](Get-Content -LiteralPath $pidFile -Raw)) -ErrorAction SilentlyContinue
        try { $correct = $correct -and $null -ne $writer -and -not $writer.HasExited }
        finally { if ($writer) { $writer.Dispose() } }
      }
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

} finally { $env:GESHER_CI_PROCESS_TRACE = $runTimed84SavedTrace }

# Additional actual GraphicsDex controls; original43 + eight reader records
# above are unchanged. Never dot-source track30's native/UI top-level probes.
$graphics84Directory = Join-Path $process84Root 'graphics'
New-Item -ItemType Directory $graphics84Directory | Out-Null
$graphics84Exe = Join-Path $graphics84Directory 'dalvikvm.exe'
$graphics84Source = Join-Path $PSScriptRoot '../fixtures/Graphics84.cs'
& $process84Compiler /nologo /warnaserror+ /platform:x64 /target:exe /main:Graphics84 "/out:$graphics84Exe" $graphics84Source $process84Source
if ($LASTEXITCODE -ne 0) {
  Write-Host 'FAIL  CI processes: actual graphics fixture compilation failed'
  $script:failed++
  return
}
$graphics84Tokens = $null; $graphics84Errors = $null
$graphics84Ast = [Management.Automation.Language.Parser]::ParseFile(
  (Join-Path $PSScriptRoot '30-graphics.ps1'), [ref]$graphics84Tokens, [ref]$graphics84Errors)
$graphics84Function = @($graphics84Ast.FindAll({ param($n)
  $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'GraphicsDex'
}, $true))
if ($graphics84Errors.Count -or $graphics84Function.Count -ne 1) { throw 'actual GraphicsDex source extraction failed' }
. ([scriptblock]::Create($graphics84Function[0].Extent.Text))
[IO.File]::WriteAllText((Join-Path $graphics84Directory 'graphics-tests.dex'), '')
$graphics84SavedDir = $Dir
$graphics84SavedEnv = @{}
foreach ($name in 'GESHER_GRAPHICS84_MODE','GESHER_GRAPHICS84_RECEIPT','GESHER_GRAPHICS84_ARGS','GESHER_GRAPHICS84_CWD','GESHER_CI_PROCESS_TRACE') {
  $graphics84SavedEnv[$name] = [Environment]::GetEnvironmentVariable($name)
}
try {
  $Dir = $graphics84Directory
  $env:GESHER_CI_PROCESS_TRACE = '1'
  $env:GESHER_GRAPHICS84_CWD = [Environment]::CurrentDirectory
  $graphics84Args = @('space argument', 'quote"argument', 'Unicode-אב')
  $graphics84Bcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
  $env:GESHER_GRAPHICS84_ARGS = Join-Path $graphics84Directory 'expected-argv.txt'
  [IO.File]::WriteAllLines($env:GESHER_GRAPHICS84_ARGS, [string[]](@("-Xbootclasspath:$graphics84Bcp",
    "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false', '-cp',
    (NoDrive "$Dir/graphics-tests.dex"), 'gesher.GraphicsFixture') + $graphics84Args))
  foreach ($mode in 'normal','nonzero','missing-marker','timeout','retained') {
    $env:GESHER_GRAPHICS84_MODE = $mode
    $receipt = Join-Path $graphics84Directory "$mode.receipt"
    $env:GESHER_GRAPHICS84_RECEIPT = $receipt
    $before = $script:failed
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
      $text = (& { GraphicsDex "fixture-$mode" GraphicsFixture $graphics84Args 1 } 6>&1 | Out-String)
      $delta = $script:failed - $before
      $script:failed = $before # Expected negative outcomes remain required.
      $expectedDelta = if ($mode -eq 'normal') { 0 } else { 1 }
      $native = [regex]::Match($text, 'kernel=True tasks=True pipes=2 disposed=True output=(\d+) error=(\d+)')
      $invalid = $false
      $correct = $delta -eq $expectedDelta -and $native.Success -and (Test-Path -LiteralPath $receipt)
      if ($native.Success) {
        $invalid = [Gtest84NativeHandles]::Closed([IntPtr][long]$native.Groups[1].Value) -and
          [Gtest84NativeHandles]::Closed([IntPtr][long]$native.Groups[2].Value)
        $correct = $correct -and $invalid
      }
      if (Test-Path -LiteralPath $receipt) {
        $actual = [IO.File]::ReadAllLines($receipt)
        $correct = $correct -and $actual.Count -eq 5 -and $actual[2] -eq $graphics84Exe -and
          $actual[3] -eq $env:GESHER_GRAPHICS84_CWD -and $actual[4] -eq 'argv-and-env-exact' -and
          $text.Contains("pid=$($actual[0]) start=$($actual[1]) expectedimage=$graphics84Exe")
      }
      if ($mode -eq 'retained') {
        $correct = $correct -and $text.Contains('did not complete within ten seconds') -and
          (Test-Path -LiteralPath ($receipt + '.child'))
        if (Test-Path -LiteralPath ($receipt + '.child')) {
          $childReceipt = [IO.File]::ReadAllLines($receipt + '.child')
          $child = Get-Process -Id ([int]$childReceipt[0]) -ErrorAction SilentlyContinue
          try {
            $correct = $correct -and $null -ne $child -and -not $child.HasExited -and
              $child.StartTime.ToUniversalTime().Ticks -eq [long]$childReceipt[1] -and $child.Path -eq $childReceipt[2]
          } finally { if ($child) { $child.Dispose() } }
        }
      }
      if ($mode -eq 'timeout') { $correct = $correct -and $text.Contains('timed out after 1 s') }
      Write-Host "ACTUAL_GRAPHICS_CONTROL case=$mode native-pipes-invalid=$invalid expected-failures=$expectedDelta actual-failures=$delta elapsed-ms=$($timer.ElapsedMilliseconds)"
      Write-Host $text
      if ($correct) { Write-Host "PASS  CI processes: GraphicsDex $mode actual helper/status/argv/ownership" }
      else { Write-Host "FAIL  CI processes: GraphicsDex $mode actual helper/status/argv/ownership"; $script:failed++ }
    } finally {
      if (Test-Path -LiteralPath ($receipt + '.child')) {
        $childReceipt = [IO.File]::ReadAllLines($receipt + '.child')
        $owned = Get-Process -Id ([int]$childReceipt[0]) -ErrorAction SilentlyContinue
        if ($owned) {
          try {
            if ($owned.Path -ne $graphics84Exe -or $owned.StartTime.ToUniversalTime().Ticks -ne [long]$childReceipt[1]) {
              throw 'graphics retained child identity mismatch'
            }
            try { $owned.Kill($true) } catch {
              Write-Host "FAIL  CI processes: graphics child kill failed: $_"
              $script:failed++
            }
            $settled = $false
            try { $settled = $owned.WaitForExit(5000) } catch {
              Write-Host "FAIL  CI processes: graphics child kernel completion unavailable: $_"
              $script:failed++
            }
            if (-not $settled) {
              Write-Host 'FAIL  CI processes: graphics child cleanup failed; same parent retains ownership'
              $script:failed++
              while (-not $settled) { try { $settled = $owned.WaitForExit(5000) } catch {} }
            }
          } finally { $owned.Dispose() }
        }
      }
    }
  }
} finally {
  $Dir = $graphics84SavedDir
  foreach ($name in $graphics84SavedEnv.Keys) { [Environment]::SetEnvironmentVariable($name, $graphics84SavedEnv[$name]) }
}
