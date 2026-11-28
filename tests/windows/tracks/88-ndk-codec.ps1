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
      # Check passes on the expected marker below, not on a returned value.
      $global:LASTEXITCODE = 0
      if (-not (Test-Path "$native/ndk-codec-test") -or
          -not (Test-Path "$native/libndk_codec_test.so") -or
          -not (Test-Path "$codecDir/ndk-codec-tests.dex")) { return 'NDK codec fixture missing' }
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
      if (-not $ok) { return "NDK codec ($abi; $mode) exit $($result.Code): $($result.Text)" }
      return "NDK codec ($abi; $mode) contract verified"
    } @("NDK codec ($abi; $mode) contract verified")
  }
}

# The real NDK AVC buffer, live-control, rendered-frame and input Surface paths
# through dalvikvm's native-library loader.
$codecDex = Join-Path $codecDir 'ndk-codec.dex'
if (-not (Test-Path $codecDex)) {
  Write-Host 'FAIL  NDK codec contract fixture DEX missing'
  $script:failed++
  return
}
foreach ($codecAbi in @(@{ Name = 'x86_64'; Triple = 'x86_64' },
                        @{ Name = 'arm64-v8a'; Triple = 'aarch64' })) {
  $codecDll = Join-Path $codecDir "libndk-codec-$($codecAbi.Triple).so"
  if (-not (Test-Path $codecDll)) {
    Write-Host "FAIL  NDK codec ($($codecAbi.Name)) guest fixture missing"
    $script:failed++
    continue
  }
  # Use the AVC backend whose live sync and bitrate controls the fixture checks.
  $codecPreviousEncoders = $env:GESHER_VIDEO_ENCODERS
  try {
    $env:GESHER_VIDEO_ENCODERS = 'soft'
    CheckTimed "NDK codec AVC ($($codecAbi.Name)): real encode/decode, buffers and lifecycle" "$Dir/dalvikvm.exe" `
      ($codecVm + @('-cp', (NoDrive $codecDex), 'gesher.NdkCodecTest', (NoDrive $codecDll))) `
      @("NDK codec ABI: $($codecAbi.Name)", 'NDK codec AVC frame pts=4294971956',
        'NDK codec AVC frame pts=4295005289', 'NDK codec AVC frame pts=4295038622',
        'NDK codec parameter keys: all passed',
        'NDK codec live controls: idr=1 output-bitrate=360000',
        'NDK codec frame rendered: valid=1 invalid=0',
        'NDK codec replace live surface: status=0',
        'NDK codec replace abandoned surface: status=-10000',
        'NDK codec surface: all passed', 'NDK codec AVC: all passed') 45 -Publish
  } finally {
    $env:GESHER_VIDEO_ENCODERS = $codecPreviousEncoders
  }
  $codecInputDll = Join-Path $codecDir "libndk-codec-input-$($codecAbi.Triple).so"
  if (-not (Test-Path $codecInputDll)) {
    Write-Host "FAIL  NDK codec input Surface ($($codecAbi.Name)) guest fixture missing"
    $script:failed++
    continue
  }
  CheckTimed "NDK codec AVC input Surface ($($codecAbi.Name)): real picture and EOS" "$Dir/dalvikvm.exe" `
    ($codecVm + @('-cp', (NoDrive $codecDex), 'gesher.NdkCodecTest', (NoDrive $codecInputDll))) `
    @('NDK codec input encoded AVC picture:', 'NDK codec input Surface: all passed',
      'NDK codec persistent input Surface: all passed') 45 -Publish
}
