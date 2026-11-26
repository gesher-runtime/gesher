# The real NDK AVC buffer path through dalvikvm's native-library loader.
$codecRoot = Join-Path (Resolve-Path $Dir).Path 'media/ndk-codec'
$codecDex = Join-Path $codecRoot 'ndk-codec.dex'
if (-not (Test-Path $codecDex)) {
  Write-Host 'FAIL  NDK codec contract fixture DEX missing'
  $script:failed++
  return
}
$codecVm = @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
  '-Xint', '-Xusejit:false')
foreach ($codecAbi in @(@{ Name = 'x86_64'; Triple = 'x86_64' },
                        @{ Name = 'arm64-v8a'; Triple = 'aarch64' })) {
  $codecDll = Join-Path $codecRoot "libndk-codec-$($codecAbi.Triple).so"
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
        'NDK codec live controls: idr=1 output-bitrate=360000',
        'NDK codec frame rendered: valid=1 invalid=0',
        'NDK codec surface: all passed', 'NDK codec AVC: all passed') 45 -Publish
  } finally {
    $env:GESHER_VIDEO_ENCODERS = $codecPreviousEncoders
  }
  $codecInputDll = Join-Path $codecRoot "libndk-codec-input-$($codecAbi.Triple).so"
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
