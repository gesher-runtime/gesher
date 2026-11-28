# Public AMediaCodec calls compiled by the Android NDK, including the 32-byte
# callback aggregate and guest callbacks, through ELF and ART in both ABIs.
$codecHost = (Resolve-Path $Dir).Path
$codecDir = Join-Path $codecHost 'media/ndk-codec'
$codecVm = @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')

function InvokeNdkCodec([string] $Exe, [string[]] $Arguments, [string] $NativeDirectory) {
  $info = [Diagnostics.ProcessStartInfo]::new($Exe)
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $info.Environment['LD_LIBRARY_PATH'] = NoDrive $NativeDirectory
  foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
  $process = [Diagnostics.Process]::Start($info)
  try {
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $timedOut = -not $process.WaitForExit(120000)
    if ($timedOut) { $process.Kill($true); $process.WaitForExit() }
    $text = ''
    if ($stdout.Wait(5000)) { $text += $stdout.Result } else { $timedOut = $true }
    if ($stderr.Wait(5000)) { $text += $stderr.Result } else { $timedOut = $true }
    return @{ Code = $(if ($timedOut) { -1 } else { $process.ExitCode }); Text = $text }
  } finally {
    $process.Dispose()
  }
}

foreach ($abi in @('x86_64', 'arm64-v8a')) {
  $native = Join-Path $codecDir $abi
  foreach ($mode in @('executable', 'JNI')) {
    Check "NDK codec ($abi; $mode): exports, AVC round trip, Opus, ownership and callback lifecycle" {
      if (-not (Test-Path "$native/ndk-codec-test") -or
          -not (Test-Path "$native/libndk_codec_test.so") -or
          -not (Test-Path "$codecDir/ndk-codec-tests.dex")) { return $false }
      if ($mode -eq 'executable') {
        $result = InvokeNdkCodec "$codecHost/gesher-exec.exe" @('--elf', "$native/ndk-codec-test", 'ndk-codec-test') $native
      } else {
        $result = InvokeNdkCodec "$codecHost/dalvikvm.exe" ($codecVm + @('-cp',
          (NoDrive "$codecDir/ndk-codec-tests.dex"), 'gesher.MediaCodecNdkTest',
          (NoDrive "$native/libndk_codec_test.so"))) $native
      }
      $ok = $result.Code -eq 0 -and $result.Text.Contains('ndk-codec: PASS (0 failures)') -and
            $result.Text.Contains("ndk-codec ABI: $abi") -and
            $result.Text.Contains("ndk-codec caller: $mode") -and
            -not $result.Text.Contains('FAIL:')
      if ($mode -eq 'JNI') { $ok = $ok -and $result.Text.Contains('MediaCodecNdkTest: all passed') }
      if (-not $ok) { Write-Host "NDK codec ($abi; $mode) exit $($result.Code): $($result.Text)" }
      return $ok
    }
  }
}
