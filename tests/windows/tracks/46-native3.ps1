# native3: libjnigraphics over hwui's real Bitmap pixels. The NDK library
# checks its dynamic entry points, direct pixel aliasing, data space, callback
# compression, errors and a PNG round trip through Java BitmapFactory.
# 44-native2.ps1 supplies N2Run, the ABI list and APK checks in both modes.

$fontWork = Join-Path $env:RUNNER_TEMP 'native-fonts-working-אב space'
$fontUnicode = Join-Path $env:RUNNER_TEMP 'native-fonts-root-אב space'
New-Item -ItemType Directory -Force $fontWork, "$fontUnicode/etc", "$fontUnicode/fonts" | Out-Null
Set-Content -LiteralPath "$fontWork/font-unrelated-canary.txt" -Value 'unrelated-canary' -Encoding ascii -NoNewline
Copy-Item -LiteralPath "$root/system/etc/fonts.xml" -Destination "$fontUnicode/etc/fonts.xml" -Force
Get-ChildItem -LiteralPath "$root/system/fonts" -File | Copy-Item -Destination "$fontUnicode/fonts" -Force
# Real canaries outside each mapped font directory make traversal controls
# reject an existing file, rather than passing because an escaped file is absent.
foreach ($fontParent in @($n2Dir, "$root/system", $fontUnicode)) {
  Set-Content -LiteralPath "$fontParent/fontscanary.txt" -Value 'font namespace boundary canary' -Encoding utf8
}
foreach ($fontAbi in $abis) {
  $fontBin = Join-Path $n2 $fontAbi.Abi
  foreach ($fontCase in @('unset', 'set', 'unicode', 'wrong')) {
    $fontMode = if ($fontCase -eq 'unicode') { 'set' } else { $fontCase }
    $fontRoot = if ($fontCase -eq 'unicode') { NoDrive $fontUnicode } else { NoDrive "$root/system" }
    $fontExpectedDir = if ($fontCase -eq 'unset') { NoDrive "$n2Dir/fonts" } else { "$fontRoot/fonts" }
    $fontExpectedXml = if ($fontCase -eq 'unset') { "$fontExpectedDir/fonts.xml" } else { "$fontRoot/etc/fonts.xml" }
    $fontOutput = N2Run "$n2Dir/gesher-exec.exe" @('--elf', "$fontBin/fonts_standalone", 'fonts_standalone', $fontMode, $fontRoot, $fontExpectedDir, $fontExpectedXml) $fontWork @{ LD_LIBRARY_PATH = (NoDrive $fontBin) } 120 | Out-String
    $fontExit = $LASTEXITCODE
    Write-Host "INFO  standalone fonts ($($fontAbi.Tag)/$fontCase): exit=$fontExit $fontOutput"
    if ($fontExit -ne 0 -or $fontOutput -notmatch "FONT_PASS case=$fontMode ") { Write-Host "FAIL  standalone fonts ($($fontAbi.Tag)/$fontCase): unavailable or failed"; $failed++ }
  }
}

if (-not (Test-Path "$n2/native2-test.dex")) {
  Write-Host 'FAIL  native3: the native test artifact is missing'
  $failed++
  return
}
foreach ($fontAbi in $abis) {
  $fontBin = Join-Path $n2 $fontAbi.Abi
  foreach ($fontCase in @('unset', 'set', 'unicode', 'wrong')) {
    $fontMode = if ($fontCase -eq 'unicode') { 'set' } else { $fontCase }
    $fontRoot = if ($fontCase -eq 'unicode') { NoDrive $fontUnicode } else { NoDrive "$root/system" }
    $fontExpectedDir = if ($fontCase -eq 'unset') { NoDrive "$n2Dir/fonts" } else { "$fontRoot/fonts" }
    $fontExpectedXml = if ($fontCase -eq 'unset') { "$fontExpectedDir/fonts.xml" } else { "$fontRoot/etc/fonts.xml" }
    Check "native3 ($($fontAbi.Tag)/$fontCase): native fonts from core-only DEX without framework font classes" {
      N2Run "$n2Dir/dalvikvm.exe" @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
        '-Xint', '-Xusejit:false', '-cp', (NoDrive "$n2/native2-test.dex"), 'gesher.Native3StandaloneFontTest',
        (NoDrive "$fontBin/libfonts_standalone.so"), $fontMode, $fontRoot, $fontExpectedDir, $fontExpectedXml) $fontWork
    } @('FONT_CORE_CLASSES_ABSENT Typeface FontFamily SystemFontMap', "FONT_PASS case=$fontMode ", "FONT_CORE_DEX_PASS case=$fontMode")
  }
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
$n3Width = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../../../probes/native3/unicode/width-reference.json') -Raw | ConvertFrom-Json
Check 'native3: wcwidth matches pinned bionic ICU4X for every code point and signed invalid input' {
  N2Run "$n2Dir/native3_unicode_test.exe" @()
} @("HOST_WIDTH wcwidth $($n3Width.wcwidth)")
Check 'native3: ART CPU clocks measure the real caller and process, including a busy worker' {
  N2Run "$n2Dir/native3_cpu_clock_test.exe" @()
} @('PASS  native3 clocks: ART current-thread CPU time matches GetThreadTimes',
  'PASS  native3 clocks: ART process CPU time matches GetProcessTimes',
  'PASS  native3 clocks: a busy worker has its own measured CPU time',
  'PASS  native3 clocks: the waiting caller keeps its own CPU clock while another thread runs',
  'PASS  native3 clocks: the process CPU clock includes worker CPU time', '0 test(s) failed')
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
