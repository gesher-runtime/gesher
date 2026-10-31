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

# Actual GraphicsDex, CI-only bridge; historical prefix43+8 remains exact.
$graphics84Source = Join-Path $PSScriptRoot '../fixtures/Graphics84.cs'
Add-Type -Path @($graphics84Source, $process84Source)
if ([IntPtr]::Size -ne 8) { throw 'Graphics84 requires64bit handles' }
$graphics84Directory = Join-Path $process84Root 'graphics'
New-Item -ItemType Directory $graphics84Directory | Out-Null
$graphics84Exe = Join-Path $graphics84Directory 'dalvikvm.exe'
& $process84Compiler /nologo /warnaserror+ /platform:x64 /target:exe /main:Graphics84 "/out:$graphics84Exe" $graphics84Source $process84Source
if ($LASTEXITCODE -ne 0) { throw 'Graphics84 CI compilation failed' }
$t=$null; $e=$null
$a=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot '30-graphics.ps1'),[ref]$t,[ref]$e)
$f=@($a.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'GraphicsDex'},$true))
if($e.Count -or $f.Count -ne 1){throw 'actual GraphicsDex extraction failed'}
. ([scriptblock]::Create($f[0].Extent.Text))
[IO.File]::WriteAllText((Join-Path $graphics84Directory 'graphics-tests.dex'),'')
$graphics84SavedDir=$Dir
$graphics84SavedEnv=@{}
foreach($n in 'GESHER_GRAPHICS84_MODE','GESHER_GRAPHICS84_RECEIPT','GESHER_GRAPHICS84_ARGS','GESHER_GRAPHICS84_CWD','GESHER_CI_PROCESS_TRACE','GESHER_GRAPHICS84_OWNER_PID','GESHER_GRAPHICS84_OWNER_FILETIME','GESHER_GRAPHICS84_NONCE','GESHER_GRAPHICS84_EXPECTED_IMAGE'){$graphics84SavedEnv[$n]=[Environment]::GetEnvironmentVariable($n)}
$Dir=$graphics84Directory
$env:GESHER_CI_PROCESS_TRACE='1'
$env:GESHER_GRAPHICS84_CWD=[Environment]::CurrentDirectory
$env:GESHER_GRAPHICS84_OWNER_PID=[string]$PID
$env:GESHER_GRAPHICS84_OWNER_FILETIME=[string][Graphics84]::Birth([Graphics84]::GetCurrentProcess())
$env:GESHER_GRAPHICS84_EXPECTED_IMAGE=$graphics84Exe
$graphics84Args=@('space argument','quote"argument','Unicode-אב')
$bcpGraphics=(('core-oj','core-libart','core-icu4j','framework','gesher-app')|ForEach-Object{NoDrive "$Dir/$_.jar"})-join ':'
$env:GESHER_GRAPHICS84_ARGS=Join-Path $graphics84Directory 'expected-argv.txt'
[IO.File]::WriteAllLines($env:GESHER_GRAPHICS84_ARGS,[string[]](@("-Xbootclasspath:$bcpGraphics","-Ximage:$(NoDrive "$root/system")/framework/boot.art",'-Xint','-Xusejit:false','-cp',(NoDrive "$Dir/graphics-tests.dex"),'gesher.GraphicsFixture')+$graphics84Args))
# Each input slot belongs to g BEFORE open/read/close, including unknown return.
# One-byte managed requests keep CUMULATIVE requested bytes <=16385 even when
# a stream lies about Length, returns partial data, or changes during the read.
$graphics84ReadBounded = {
  param($slot,[string]$path)
  if($slot.Open -ne 'UNATTEMPTED'){throw 'uncertain input acquisition retained'}
  $slot.Open='UNKNOWN'
  $slot.Stream=[IO.File]::OpenRead($path)
  $slot.Open='OWNED'
  while($slot.Requested -lt 16385){
    $slot.Read='UNKNOWN'
    $slot.Requested++
    $n=$slot.Stream.Read($slot.Buffer,$slot.Count,1)
    if($n -lt 0 -or $n -gt 1){throw 'invalid managed stream read count'}
    $slot.Read='DONE'
    if($n -eq 0){$slot.Eof=$true;break}
    $slot.Count+=$n
    if($slot.Count -eq 16385){break}
  }
  $slot.Close='FAILED'
  $slot.Stream.Dispose()
  $slot.Close='DONE'
  if(-not $slot.Eof -or $slot.Count -gt 16384){throw 'input exceeds acquisition cap or lacks bounded EOF'}
  $slot.Text=[Text.UTF8Encoding]::new($false,$true).GetString($slot.Buffer,0,$slot.Count)
}
foreach($mode in 'normal','nonzero','missing-marker','timeout','retained'){
  # Pending receiver root precedes launch/sinks. A partial record never clears it.
  $g=@{Phase='Launch';Failed=$false;Pending=($mode -eq 'retained');Receipt='UNATTEMPTED';Query='UNATTEMPTED';Wait='UNATTEMPTED';Kill=$false;Closed=$false;Slot=[Graphics84+Slot]::new();Obs=$null;Text='';Document=$null;DocClose='UNATTEMPTED';Delta=0;Before=$script:failed;Inputs=@{};ParentReceipt='UNATTEMPTED';ParentDocument=$null;ParentDocClose='UNATTEMPTED';ParentBound=$false;ChildNonsignaled=$false}
  foreach($name in 'Parent','Child'){$g.Inputs[$name]=@{Open='UNATTEMPTED';Stream=$null;Read='UNATTEMPTED';Close='UNATTEMPTED';Buffer=[byte[]]::new(16385);Count=0;Requested=0;Eof=$false;Text=$null}}
  $env:GESHER_GRAPHICS84_MODE=$mode
  $env:GESHER_GRAPHICS84_NONCE=[guid]::NewGuid().ToString('N')+[guid]::NewGuid().ToString('N')
  $env:GESHER_GRAPHICS84_RECEIPT=Join-Path $graphics84Directory "$mode.receipt"
  while($true){
    try{
      if($g.Phase -eq 'Launch'){
        $g.Phase='UNKNOWN_LAUNCH'
        $script:GraphicsDexObservation=$null
        $g.Text=(& {GraphicsDex "fixture-$mode" GraphicsFixture $graphics84Args 1} 6>&1|Out-String)
        $g.Obs=$script:GraphicsDexObservation
        $g.Delta=$script:failed-$g.Before
        $script:failed=$g.Before
        $g.Phase='ParentReceipt'
      }
      if($g.Phase -eq 'ParentReceipt'){
        if($g.ParentReceipt -ne 'UNATTEMPTED'){throw 'uncertain parent receipt retained'}
        $g.ParentReceipt='FAILED'
        & $graphics84ReadBounded $g.Inputs.Parent $env:GESHER_GRAPHICS84_RECEIPT
        $g.ParentDocument=[Text.Json.JsonDocument]::Parse($g.Inputs.Parent.Text)
        $parent=$g.ParentDocument.RootElement
        if($parent.ValueKind -ne [Text.Json.JsonValueKind]::Object){throw 'parent object required'}
        $keys=@('schema','source','case','nonce','pid','creation_filetime','expected_image','cwd','argv_env')
        $r=@{}
        foreach($v in $parent.EnumerateObject()){
          if($v.Name -cnotin $keys -or $r.ContainsKey($v.Name)){throw 'parent unknown/duplicate key'}
          $r[$v.Name]=$v.Value
        }
        if($r.Count -ne $keys.Count){throw 'parent missing key'}
        foreach($k in $keys|Where-Object{$_ -ne 'pid'}){if($r[$k].ValueKind -ne [Text.Json.JsonValueKind]::String){throw 'parent string type required'}}
        if($r.pid.ValueKind -ne [Text.Json.JsonValueKind]::Number -or $r.pid.GetUInt32() -eq 0){throw 'parent PID type/range'}
        $birth=[UInt64]0;$birthText=$r.creation_filetime.GetString()
        if(-not [UInt64]::TryParse($birthText,[ref]$birth) -or [string]$birth -cne $birthText){throw 'parent canonical raw FILETIME required'}
        $obs=$g.Obs
        if($null -eq $obs -or $obs.Source -cne $env:GITHUB_SHA -or $env:GITHUB_SHA -cnotmatch '^[0-9a-f]{40}$' -or $obs.Case -cne $mode -or $obs.Nonce -cne $env:GESHER_GRAPHICS84_NONCE -or $env:GESHER_GRAPHICS84_NONCE -cnotmatch '^[0-9a-f]{64}$' -or -not $obs.KernelComplete -or -not $obs.Released -or -not $obs.TasksComplete -or $obs.PipesClosed -ne 2 -or $null -eq $obs.NativeExitCode -or $obs.ExpectedImage -cne $graphics84Exe){throw 'all-case typed parent proof rejected'}
        if($r.schema.GetString() -cne 'GRAPHICS84_PARENT_V1' -or $r.source.GetString() -cne $env:GITHUB_SHA -or $r.case.GetString() -cne $mode -or $r.nonce.GetString() -cne $env:GESHER_GRAPHICS84_NONCE -or $r.pid.GetUInt32() -ne $obs.Pid -or $birth -ne $obs.CreationFiletime -or $r.expected_image.GetString() -cne $graphics84Exe -or $r.cwd.GetString() -cne $env:GESHER_GRAPHICS84_CWD -or $r.argv_env.GetString() -cne 'EXACT'){throw 'parent receipt case/process/argv-env rejected'}
        $g.ParentDocClose='FAILED';$g.ParentDocument.Dispose();$g.ParentDocClose='DONE'
        $g.ParentReceipt='DONE';$g.ParentBound=$true
        $g.Phase=if($g.Pending){'Receipt'}else{'Report'}
      }
      if($g.Phase -eq 'Receipt'){
        if($g.Receipt -ne 'UNATTEMPTED'){throw 'uncertain receipt retained'}
        $g.Receipt='FAILED'
        & $graphics84ReadBounded $g.Inputs.Child ($env:GESHER_GRAPHICS84_RECEIPT+'.child')
        $g.Document=[Text.Json.JsonDocument]::Parse($g.Inputs.Child.Text)
        $rootJson=$g.Document.RootElement
        if($rootJson.ValueKind -ne [Text.Json.JsonValueKind]::Object){throw 'object required'}
        $keys=@('schema','source','case','nonce','bits','parent_pid','parent_creation_filetime','fixture_pid','fixture_creation_filetime','child_pid','child_creation_filetime','destination_handle_hex16','duplicate_state','expected_create_application_path','parent_access','child_access','inherit','options','publication_phase','source_prepublication_releases','source_child_escrow')
        $r=@{}
        foreach($property in $rootJson.EnumerateObject()){
          if($property.Name -cnotin $keys -or $r.ContainsKey($property.Name)){throw 'unknown/duplicate key'}
          $r[$property.Name]=$property.Value
        }
        if($r.Count -ne $keys.Count){throw 'missing key'}
        foreach($k in $keys|Where-Object{$_ -notin @('bits','parent_pid','fixture_pid','child_pid','inherit','options','source_prepublication_releases')}){if($r[$k].ValueKind -ne [Text.Json.JsonValueKind]::String){throw 'string type required'}}
        foreach($k in 'bits','parent_pid','fixture_pid','child_pid','options'){if($r[$k].ValueKind -ne [Text.Json.JsonValueKind]::Number){throw 'integer type required'}}
        foreach($k in 'parent_pid','fixture_pid','child_pid'){if($r[$k].GetUInt32() -eq 0){throw 'positive PID required'}}
        $releases=@{}
        if($r.source_prepublication_releases.ValueKind -ne [Text.Json.JsonValueKind]::Object){throw 'release object required'}
        foreach($v in $r.source_prepublication_releases.EnumerateObject()){
          if($v.Name -cnotin @('thread','output','error','target') -or $releases.ContainsKey($v.Name) -or $v.Value.ValueKind -ne [Text.Json.JsonValueKind]::String -or $v.Value.GetString() -cne 'DONE'){throw 'release facts rejected'}
          $releases[$v.Name]=$true
        }
        if($releases.Count -ne 4){throw 'release keyset'}
        foreach($k in 'parent_creation_filetime','fixture_creation_filetime','child_creation_filetime'){
          $v=$r[$k].GetString(); $u=[UInt64]0
          if(-not [UInt64]::TryParse($v,[ref]$u) -or [string]$u -cne $v){throw 'canonical FILETIME required'}
        }
        if($r.schema.GetString() -cne 'GRAPHICS84_HANDLE_TRANSFER_V1' -or $r.source.GetString() -cne $env:GITHUB_SHA -or $env:GITHUB_SHA -cnotmatch '^[0-9a-f]{40}$' -or $r.case.GetString() -cne 'retained' -or $r.nonce.GetString() -cne $env:GESHER_GRAPHICS84_NONCE -or $env:GESHER_GRAPHICS84_NONCE -cnotmatch '^[0-9a-f]{64}$' -or $r.bits.GetUInt32() -ne 64 -or $r.parent_pid.GetUInt32() -ne $PID -or $r.parent_creation_filetime.GetString() -cne $env:GESHER_GRAPHICS84_OWNER_FILETIME -or $r.parent_access.GetString() -cne '0x1040' -or $r.child_access.GetString() -cne '0x101001' -or $r.inherit.ValueKind -ne [Text.Json.JsonValueKind]::False -or $r.options.GetUInt32() -ne 0 -or $r.publication_phase.GetString() -cne 'TRANSFER_PUBLISHED' -or $r.duplicate_state.GetString() -cne 'RETURNED_OWNED_TARGET' -or $r.source_child_escrow.GetString() -cne 'REQUIRES_EXACT_PARENT_NATIVE_EXIT0' -or $r.expected_create_application_path.GetString() -cne $graphics84Exe){throw 'case/source/authority/role mismatch'}
        $obs=$g.Obs
        if($null -eq $obs -or $obs.Source -cne $env:GITHUB_SHA -or $obs.Case -cne 'retained' -or $obs.Nonce -cne $env:GESHER_GRAPHICS84_NONCE -or -not $obs.KernelComplete -or -not $obs.Released -or $null -eq $obs.NativeExitCode -or $obs.NativeExitCode -ne 0 -or $obs.KillReserved -or $obs.Pid -ne $r.fixture_pid.GetUInt32() -or [string]$obs.CreationFiletime -cne $r.fixture_creation_filetime.GetString()){throw 'source parent native completion rejected'}
        $hex=$r.destination_handle_hex16.GetString()
        if($hex -cnotmatch '^[0-9a-f]{16}$' -or $hex -in @('0000000000000000','ffffffffffffffff','fffffffffffffffe','fffffffffffffffd','fffffffffffffffc','fffffffffffffffb','fffffffffffffffa')){throw 'fullwidth target handle rejected'}
        $bits=[Convert]::ToUInt64($hex,16)
        $g.Slot.Handle=[IntPtr]::new([BitConverter]::ToInt64([BitConverter]::GetBytes($bits),0))
        $g.Query='FAILED'
        $candidatePid=[Graphics84]::GetProcessId($g.Slot.Handle); $nativeError=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
        if($candidatePid -eq 0 -or $candidatePid -ne $r.child_pid.GetUInt32() -or [string][Graphics84]::Birth($g.Slot.Handle) -cne $r.child_creation_filetime.GetString()){throw "receiver native identity rejected:$nativeError"}
        $g.Slot.State='OWNED';$g.Query='DONE';$g.Receipt='DONE'
        $g.DocClose='FAILED';$g.Document.Dispose();$g.DocClose='DONE'
        $g.Phase='Settle'
      }
      if($g.Phase -eq 'Settle'){
        if($g.Wait -eq 'FAILED'){throw 'uncertain receiver wait retained'}
        if($g.Wait -eq 'UNATTEMPTED'){
          $g.Wait='FAILED'
          $w=[Graphics84]::WaitForSingleObject($g.Slot.Handle,0);$err=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
          if($w -ne 0 -and $w -ne 258){throw "receiver liveness unavailable:$err"}
          if($w -eq 0){$g.Failed=$true} # original live-writer assertion required
          if($w -eq 258){
            $g.ChildNonsignaled=$true
            $g.Kill=$true
            $ok=[Graphics84]::TerminateProcess($g.Slot.Handle,1);$err=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
            if(-not $ok){$g.Failed=$true} # reservation is never retried
          }
        }
        $g.Wait='FAILED'
        $w=[Graphics84]::WaitForSingleObject($g.Slot.Handle,5000);$err=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
        if($w -eq 258){$g.Failed=$true;$g.Wait='PASSIVE';continue}
        if($w -ne 0){throw "receiver kernel unavailable:$err"}
        $g.Wait='DONE';$g.Phase='Close'
      }
      if($g.Phase -eq 'Close'){
        [Graphics84]::Close($g.Slot)
        $g.Closed=$true;$g.Pending=$false;$g.Phase='Report'
      }
      if($g.Phase -eq 'Report'){
        $obs=$g.Obs
        $facts=$g.ParentBound -and $g.ParentReceipt -eq 'DONE' -and $null -ne $obs -and
          $obs.Source -ceq $env:GITHUB_SHA -and $obs.Case -ceq $mode -and $obs.Nonce -ceq $env:GESHER_GRAPHICS84_NONCE -and
          $obs.Pid -gt 0 -and $obs.CreationFiletime -gt 0 -and $obs.KernelComplete -and $obs.Released -and
          $obs.TasksComplete -and $obs.PipesClosed -eq 2 -and $obs.ExpectedImage -ceq $graphics84Exe -and
          $obs.NativeExitCode -notin @(91,92,93,94) -and $obs.MechanismFailure -is [bool] -and
          $obs.CleanupBudgetExpired -is [bool] -and -not $obs.MechanismFailure -and -not $obs.CleanupBudgetExpired
        $expected=if($mode -eq 'normal'){0}else{1}
        $facts=$facts -and $g.Delta -eq $expected
        switch($mode){
          normal {$facts=$facts -and $obs.NativeExitCode -eq 0 -and $obs.MarkerPresent -and -not $obs.OperationExpired -and $obs.EofComplete -and -not $obs.KillReserved -and -not $obs.ObservationFailed}
          nonzero {$facts=$facts -and $obs.NativeExitCode -eq 23 -and $obs.MarkerPresent -and -not $obs.OperationExpired -and $obs.EofComplete -and -not $obs.KillReserved -and -not $obs.ObservationFailed}
          'missing-marker' {$facts=$facts -and $obs.NativeExitCode -eq 0 -and -not $obs.MarkerPresent -and -not $obs.OperationExpired -and $obs.EofComplete -and -not $obs.KillReserved -and -not $obs.ObservationFailed}
          timeout {$facts=$facts -and -not $obs.MarkerPresent -and $obs.OperationExpired -and $obs.KillReserved -and $obs.ObservationFailed -and $obs.EofComplete -and $null -ne $obs.NativeExitCode}
          retained {$facts=$facts -and $obs.NativeExitCode -eq 0 -and -not $obs.OperationExpired -and -not $obs.KillReserved -and -not $obs.EofComplete -and $obs.ObservationFailed -and $g.ChildNonsignaled -and $g.Closed -and -not $g.Pending}
          default {$facts=$false}
        }
        if(-not $facts){$g.Failed=$true}
        $pair=[regex]::Match($g.Text,'kernel=True tasks=True pipes=2 disposed=True output=(\d+) error=(\d+)')
        if(-not $pair.Success){$g.Failed=$true}
        elseif(-not [Gtest84NativeHandles]::Closed([IntPtr][long]$pair.Groups[1].Value) -or
               -not [Gtest84NativeHandles]::Closed([IntPtr][long]$pair.Groups[2].Value)){$g.Failed=$true}
        if($mode -eq 'retained' -and -not $g.Text.Contains('did not complete within ten seconds')){$g.Failed=$true}
        if($g.Failed){Write-Host "FAIL  CI processes: GraphicsDex $mode ownership/status";$script:failed++}
        else{Write-Host "PASS  CI processes: GraphicsDex $mode actual helper/typed ownership"}
        Write-Host $g.Text
        break
      }
    }catch{
      $g.Failed=$true
      # No sink, query, file, close or root-release is needed to park this owner.
      try{[Threading.Thread]::Sleep(5000)}catch{}
    }
  }
}
# Restore surrounding fixture environment through a protected owner boundary.
while($true){try{$Dir=$graphics84SavedDir;foreach($n in $graphics84SavedEnv.Keys){[Environment]::SetEnvironmentVariable($n,$graphics84SavedEnv[$n])};break}catch{try{[Threading.Thread]::Sleep(5000)}catch{}}}
