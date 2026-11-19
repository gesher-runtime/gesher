# Exercise the public NDK ABI, including guest-to-host-to-guest source callbacks.
Check 'AMediaExtractor and AMediaDataSource through both ELF ABIs' {
  $native = Join-Path (Resolve-Path $Dir).Path 'media/ndk-extractor'
  $exe = "$Dir/gesher-exec.exe"
  foreach ($abi in @('x86_64', 'aarch64')) {
    $input = "$native/ndk-extractor-$abi"
    if (-not (Test-Path $input)) { return $false }
    $info = [Diagnostics.ProcessStartInfo]::new($exe)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($arg in @('--elf', $input, 'ndk-extractor', (NoDrive "$native/embedded.bin"),
                        (NoDrive "$native/source.wav"), (NoDrive "$native/multitrack.mp4"))) { $info.ArgumentList.Add($arg) }
    $process = [Diagnostics.Process]::Start($info)
    try {
      $stdout = $process.StandardOutput.ReadToEndAsync()
      $stderr = $process.StandardError.ReadToEndAsync()
      if (-not $process.WaitForExit(60000)) {
        $process.Kill($true)
        $process.WaitForExit()
        return $false
      }
      if (-not $stdout.Wait(5000) -or -not $stderr.Wait(5000)) { return $false }
      $text = $stdout.Result + $stderr.Result
      if ($process.ExitCode -ne 0 -or $text -notmatch 'NDK extractor: .* passed') {
        Write-Host "NDK extractor (${abi}): $text"
        return $false
      }
    } finally {
      $process.Dispose()
    }
  }
  return $true
}
