# native3: libjnigraphics over hwui's real Bitmap pixels. The NDK library
# checks its dynamic entry points, direct pixel aliasing, data space, callback
# compression, errors and a PNG round trip through Java BitmapFactory.
# 44-native2.ps1 supplies N2Run, the ABI list and APK checks in both modes.

if (-not (Test-Path "$n2/native2-test.dex")) {
  Write-Host 'FAIL  native3: the native test artifact is missing'
  $failed++
  return
}
$n3Bcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') |
  ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
foreach ($a in @(@{ Abi = 'x86_64'; Tag = 'x86_64' }, @{ Abi = 'arm64-v8a'; Tag = 'arm64' })) {
  Check "native3 ($($a.Tag)): libjnigraphics direct Bitmap pixels, data space, PNG callback round trip and errors through dalvikvm" {
    N2Run "$n2Dir/dalvikvm.exe" @("-Xbootclasspath:$n3Bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
      '-Xint', '-Xusejit:false', '-cp', (NoDrive "$n2/native2-test.dex"), 'gesher.Native3BitmapTest',
      (NoDrive "$n2/$($a.Abi)/libnative2.so"))
  } @('native2 bitmap: ok; direct pixels and PNG callback round trip', 'native3 Java Bitmap sees native pixels: true')
}
