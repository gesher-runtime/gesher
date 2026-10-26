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
$n3Unicode = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../../../probes/native3/unicode/reference.json') -Raw | ConvertFrom-Json
$n3UnicodeExpected = foreach ($property in $n3Unicode.properties.PSObject.Properties) {
  "HOST_UNICODE $($property.Name) $($property.Value)"
  "HOST_UNICODE_L $($property.Name) $($property.Value)"
}
Check 'native3: every Unicode code point and invalid sentinel matches pinned bionic ICU4X, plain and locale forms' {
  N2Run "$n2Dir/native3_unicode_test.exe" @()
} $n3UnicodeExpected
Check 'native3: Process proc natives use real procfs, full-width values and the Android field format' {
  N2Run "$n2Dir/dalvikvm.exe" @("-Xbootclasspath:$n3Bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
    '-Xint', '-Xusejit:false', '-cp', (NoDrive "$n2/native2-test.dex"), 'gesher.Native3ProcessTest',
    (NoDrive "$root/native3-proc-lines.txt"))
} @('real thread/RSS counters', '64-bit decimal parsing', 'missing file preserves',
  'shared output index', 'parens, quotes, combined delimiters', 'truncated input returns false',
  'readProcFile grows', 'reads actual procfs memory counters', 'native3 process: all passed')
foreach ($a in @(@{ Abi = 'x86_64'; Tag = 'x86_64' }, @{ Abi = 'arm64-v8a'; Tag = 'arm64' })) {
  Check "native3 ($($a.Tag)): libjnigraphics direct Bitmap pixels, data space, PNG callback round trip and errors through dalvikvm" {
    N2Run "$n2Dir/dalvikvm.exe" @("-Xbootclasspath:$n3Bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
      '-Xint', '-Xusejit:false', '-cp', (NoDrive "$n2/native2-test.dex"), 'gesher.Native3BitmapTest',
      (NoDrive "$n2/$($a.Abi)/libnative2.so"))
  } @('native2 bitmap: ok; direct pixels and PNG callback round trip', 'native3 Java Bitmap sees native pixels: true')
  Check "native3 ($($a.Tag)): NDK fonts enumerate real files and match mixed-script UTF-16 runs through Minikin" {
    N2Run "$n2Dir/dalvikvm.exe" @("-Xbootclasspath:$n3Bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
      '-Xint', '-Xusejit:false', '-cp', (NoDrive "$n2/native2-test.dex"), 'gesher.Native3FontTest',
      (NoDrive "$n2/$($a.Abi)/libnative2.so"))
  } @('native2 fonts: ok;', 'mixed-script run(s)', 'shipped font(s)')
}
