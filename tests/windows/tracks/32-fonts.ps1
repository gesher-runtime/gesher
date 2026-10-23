# The system fonts (docs/fonts.md). Dot-sourced by run-tests.ps1 after
# 30-graphics.ps1, whose GraphicsDex runs the test: Check, RunDex, NoDrive,
# $Dir, $root, $bcp and $failed are its.
#
# The build's fonts/ (tools/fonts/collect-fonts.sh) installed as Android keeps
# them: the font files in $ANDROID_ROOT/fonts, fonts.xml in $ANDROID_ROOT/etc.
# Checks after this one (gesher-app runs) find the fonts there too.
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
  $size = (Get-ChildItem (Join-Path $root 'system/fonts') | Measure-Object Length -Sum).Sum
  Write-Host ("INFO  fonts: {0} files, {1:N1} MB in `$ANDROID_ROOT/fonts" -f (Get-ChildItem (Join-Path $root 'system/fonts')).Count, ($size / 1MB))
  # Typeface.DEFAULT and the families from fonts.xml, through the SharedMemory
  # font map; Latin (Roboto) and Hebrew (Noto Sans Hebrew, from the fallback
  # chain) drawn and measured through minikin; bold, italic, serif, monospace.
  GraphicsDex 'system fonts: Typeface.DEFAULT, fonts.xml families, Hebrew through the fallback chain, bold/italic' FontsTest
}
