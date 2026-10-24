# The system fonts (docs/fonts.md). Dot-sourced by run-tests.ps1 after
# 30-graphics.ps1, whose GraphicsDex runs the test: Check, RunDex, NoDrive,
# $Dir, $root, $bcp and $failed are its.
#
# The build's fonts/ (tools/fonts/collect-fonts.sh) installed as Android keeps
# them: the font files in $ANDROID_ROOT/fonts, fonts.xml in $ANDROID_ROOT/etc.
# Checks after this one (gesher-app runs) find the fonts there too, and Windows'
# own fonts through $GESHER_WINDOWS_FONTS.
$fontsDir = Join-Path $Dir 'fonts'
if (-not (Test-Path (Join-Path $fontsDir 'fonts.xml'))) {
  Write-Host "FAIL  fonts: $fontsDir/fonts.xml is missing from the build"
  $failed++
} elseif (-not (Get-Command GraphicsDex -ErrorAction SilentlyContinue)) {
  Write-Host 'FAIL  fonts: GraphicsDex (30-graphics.ps1) is not defined'
  $failed++
} else {
  foreach ($d in 'system/fonts', 'system/etc') { New-Item -ItemType Directory -Force (Join-Path $root $d) | Out-Null }
  Copy-Item (Join-Path $fontsDir '*.ttf') (Join-Path $root 'system/fonts')
  Copy-Item (Join-Path $fontsDir 'fonts.xml') (Join-Path $root 'system/etc/fonts.xml')
  # Windows' own fonts, used in place (windows-fonts.xml; ADR 0044). The framework keeps
  # drive-less paths (ADR 0010), so a font folder on another drive than the test root's
  # (C:\Windows\Fonts, the root on D:) is reached through a junction on this drive, named
  # by $GESHER_WINDOWS_FONTS; on Windows' drive (an installed Gesher) none is needed.
  if (Test-Path (Join-Path $fontsDir 'windows-fonts.xml')) {
    Copy-Item (Join-Path $fontsDir 'windows-fonts.xml') (Join-Path $root 'system/etc/windows-fonts.xml')
  }
  $windowsFonts = Join-Path $env:WINDIR 'Fonts'
  if ((Split-Path -Qualifier $windowsFonts) -ne (Split-Path -Qualifier $root)) {
    $link = Join-Path $root 'windows-fonts'
    if (-not (Test-Path $link)) { New-Item -ItemType Junction -Path $link -Target $windowsFonts | Out-Null }
    $env:GESHER_WINDOWS_FONTS = NoDrive $link
  }
  $cjk = @('msyh.ttc', 'msjh.ttc', 'YuGothR.ttc', 'malgun.ttf', 'seguiemj.ttf', 'seguisym.ttf', 'Nirmala.ttc', 'Nirmala.ttf') |
    ForEach-Object { "$_ $(if (Test-Path (Join-Path $windowsFonts $_)) { 'yes' } else { 'no' })" }
  Write-Host "INFO  fonts: Windows' fonts in $windowsFonts ($((Get-ChildItem $windowsFonts -File).Count) files): $($cjk -join ', ')"
  $size = (Get-ChildItem (Join-Path $root 'system/fonts') | Measure-Object Length -Sum).Sum
  Write-Host ("INFO  fonts: {0} files, {1:N1} MB in `$ANDROID_ROOT/fonts" -f (Get-ChildItem (Join-Path $root 'system/fonts')).Count, ($size / 1MB))
  # Typeface.DEFAULT and the families from fonts.xml, through the SharedMemory
  # font map; Latin (Roboto) and Hebrew (Noto Sans Hebrew, from the fallback
  # chain) drawn and measured through minikin; bold, italic, serif, monospace.
  GraphicsDex 'system fonts: Typeface.DEFAULT, fonts.xml families, Hebrew through the fallback chain, bold/italic' FontsTest
  # Further scripts (ADR 0044): Arabic shaped and joined, Devanagari, Thai (bundled
  # Noto), Cyrillic and Greek (Roboto); CJK by locale, color emoji and symbols from
  # Windows' fonts in place; none of them tofu.
  GraphicsDex 'further scripts: Arabic joined and right to left, Devanagari conjuncts, Thai, Cyrillic, Greek (bundled fonts); CJK, color emoji, symbols from Windows'' fonts in place, not tofu' ScriptsTest
}
