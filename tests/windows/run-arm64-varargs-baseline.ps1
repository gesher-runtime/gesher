# Diagnostic evidence only: a passing job means the baseline defect and the
# separate deep-frame control were actually observed, not product acceptance.
param([Parameter(Mandatory)] [string] $Dir)
$ErrorActionPreference = 'Stop'
$env:GESHER_ARM64_VARARGS_TRACE = '1'
foreach ($mode in 'deep', 'shallow') {
  $info = [Diagnostics.ProcessStartInfo]::new((Join-Path $Dir 'arm64_elf_test.exe'))
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $info.ArgumentList.Add((Join-Path $Dir 'arm64_varargs_baseline.so'))
  $info.ArgumentList.Add($mode)
  $process = [Diagnostics.Process]::new()
  $process.StartInfo = $info
  try {
    if (-not $process.Start()) { throw 'Baseline child did not start' }
    $out = $process.StandardOutput.ReadToEndAsync()
    $err = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(15000)) {
      $process.Kill($true)
      $process.WaitForExit()
      throw "Baseline $mode exceeded 15 seconds"
    }
    $text = $out.GetAwaiter().GetResult() + $err.GetAwaiter().GetResult()
    Write-Host "ACTUAL_VARARGS_BASELINE mode=$mode exit=$($process.ExitCode)"
    Write-Host $text
    $bounds = [regex]::Match($text,
      'ARM_VARARGS_BOUNDS sp=([0-9a-f]+) stack=([0-9a-f]+) top=([0-9a-f]+) base=([0-9a-f]+) gp=(\d+) fp=(\d+) eager_bytes=(\d+) eager_end=([0-9a-f]+)')
    if (-not $bounds.Success) { throw "Missing actual va_list bounds for $mode" }
    $top = [Convert]::ToUInt64($bounds.Groups[3].Value, 16)
    $end = [Convert]::ToUInt64($bounds.Groups[8].Value, 16)
    if ($bounds.Groups[5].Value -ne '7' -or $bounds.Groups[7].Value -ne '496') {
      throw 'The valid literal call did not take the expected baseline conversion path'
    }
    if ($mode -eq 'deep') {
      if ($process.ExitCode -ne 0 -or $end -gt $top -or
          $text -notmatch 'surfacegl: invalidation: abi arm64-v8a' -or
          $text -notmatch 'arm64_elf_test all passed') { throw 'Deep control failed' }
      Write-Host 'OBSERVED_VARARGS_BASELINE deep valid-call control completed within guest stack bounds'
    } else {
      if ($process.ExitCode -ne 139 -or $end -le $top -or
          $text -notmatch 'Cause: guest read' -or $text -match 'arm64_elf_test all passed') {
        throw 'Shallow baseline did not reproduce the specific out-of-bounds eager copy'
      }
      Write-Host 'OBSERVED_VARARGS_BASELINE shallow valid literal faults with eager copy beyond guest stack top'
    }
  } finally {
    if (-not $process.HasExited) { $process.Kill($true); $process.WaitForExit() }
    $process.Dispose()
  }
}
