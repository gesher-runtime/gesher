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
