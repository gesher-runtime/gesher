param([Parameter(Mandatory)] [string] $Dir)
$ErrorActionPreference = 'Stop'
$env:GESHER_ARM64_VARARGS_TRACE = '1'

function InvokeVarargs([string] $Exe, [string[]] $Arguments, [int] $ExpectedExit = 0) {
  $info = [Diagnostics.ProcessStartInfo]::new($Exe)
  $info.UseShellExecute = $false
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
  $process = [Diagnostics.Process]::new()
  $process.StartInfo = $info
  $started = $false
  try {
    if (-not $process.Start()) { throw 'Varargs child did not start' }
    $started = $true
    $out = $process.StandardOutput.ReadToEndAsync()
    $err = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(60000)) {
      $process.Kill($true)
      if (-not $process.WaitForExit(2000)) { throw 'Varargs child exceeded 60 seconds; termination did not complete within 2 seconds (124)' }
      throw 'Varargs child exceeded 60 seconds (124)'
    }
    if (-not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]] @($out, $err), 5000)) {
      throw 'Varargs output pipes did not reach EOF within 5 seconds (124)'
    }
    $text = $out.GetAwaiter().GetResult() + $err.GetAwaiter().GetResult()
    Write-Host "ACTUAL_VARARGS_EXIT code=$($process.ExitCode) expected=$ExpectedExit executable=$Exe"
    Write-Host $text
    if ($process.ExitCode -ne $ExpectedExit) { throw 'Unexpected actual varargs child exit' }
    return $text
  } finally {
    if ($started -and -not $process.HasExited) {
      $process.Kill($true)
      if (-not $process.WaitForExit(2000)) { Write-Host 'FAIL arm64 varargs: cleanup did not terminate child within 2 seconds' }
    }
    $process.Dispose()
  }
}

$smoke = InvokeVarargs (Join-Path $Dir 'arm64_smoke_test.exe') @()
if ($smoke -notmatch 'arm64_smoke_test all passed' -or
    @([regex]::Matches($smoke, 'incompatible host va_list ABI rejected\s+ok')).Count -ne 4 -or
    $smoke -notmatch 'null host rejected before initialization\s+ok') { throw 'Missing API rejection/smoke proof' }
foreach ($mode in 'shallow', 'deep') {
  $text = InvokeVarargs (Join-Path $Dir 'arm64_elf_test.exe') @((Join-Path $Dir 'arm64_varargs_baseline.so'), $mode)
  if ($text -notmatch 'surfacegl: invalidation: abi arm64-v8a' -or
      $text -notmatch 'arm64_elf_test all passed' -or $text -notmatch 'eager_bytes=0') {
    throw "Original $mode logger did not complete without an eager stack read"
  }
  Write-Host "PASS arm64 varargs: original $mode Surface printf/vprintf body returns unchanged"
}
foreach ($abi in 'x86_64', 'arm64-v8a') {
  $text = InvokeVarargs (Join-Path $Dir 'gesher-exec.exe') @('--elf', (Join-Path $Dir "$abi/arm64_varargs_test"), 'arm64-varargs-test', $env:RUNNER_TEMP)
  if ($text -notmatch 'arm64 varargs: all passed; literal/few/many GP/FP; ordered shared overflow; va_copy; nested/thread; wide and binary128 source; gzvprintf retry') {
    throw "Missing complete $abi actual formatted-output contracts"
  }
  Write-Host "PASS arm64 varargs ($abi): actual ELF formatted-output, overflow order, copied lists, nested/thread, wide/binary128 source and gzip retry"
}
