# Focused CI only: run the unchanged track80 and86 bodies against one source.
param([Parameter(Mandatory)] [string] $Dir)
$ErrorActionPreference = 'Stop'
$Dir = (Resolve-Path -LiteralPath $Dir).Path
$failed = 0

function NoDrive([string] $p) { ($p -replace '\\', '/') -replace '^[A-Za-z]:', '' }

foreach ($file in 'dalvikvm.exe', 'gesher-exec.exe', 'gesher_arm64.dll',
    'surface_invalidation_test.exe', 'surfacegl.apk', 'surfacegl-arm64.apk',
    'core-oj.jar', 'core-libart.jar', 'core-icu4j.jar', 'framework.jar', 'gesher-app.jar',
    'framework-res.apk', 'build.prop', 'framework/x86_64/boot.art', 'fonts/fonts.xml') {
  if (-not (Test-Path -LiteralPath (Join-Path $Dir $file))) {
    throw "Missing focused component artifact: $file"
  }
}
$root = Join-Path $env:RUNNER_TEMP "gesher-surface-component-$([guid]::NewGuid().ToString('N'))"
foreach ($directory in 'system/framework/x86_64', 'system/fonts', 'system/etc',
    'art', 'data', 'i18n/etc/icu', 'tzdata') {
  New-Item -ItemType Directory -Path (Join-Path $root $directory) -Force | Out-Null
}
Copy-Item "$Dir/icudt*l.dat" (Join-Path $root 'i18n/etc/icu')
Copy-Item "$Dir/framework/x86_64/*" (Join-Path $root 'system/framework/x86_64')
Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework')
Copy-Item "$Dir/build.prop" (Join-Path $root 'system')
Copy-Item "$Dir/fonts/*.ttf" (Join-Path $root 'system/fonts')
Copy-Item "$Dir/fonts/fonts.xml" (Join-Path $root 'system/etc/fonts.xml')
if (Test-Path -LiteralPath "$Dir/fonts/windows-fonts.xml") {
  Copy-Item "$Dir/fonts/windows-fonts.xml" (Join-Path $root 'system/etc/windows-fonts.xml')
}
if (Test-Path -LiteralPath "$Dir/tzdata") {
  Copy-Item -Recurse -Force "$Dir/tzdata/*" (Join-Path $root 'tzdata')
}
$windowsFonts = Join-Path $env:WINDIR 'Fonts'
if ((Split-Path -Qualifier $windowsFonts) -ne (Split-Path -Qualifier $root)) {
  $fontLink = Join-Path $root 'windows-fonts'
  New-Item -ItemType Junction -Path $fontLink -Target $windowsFonts | Out-Null
  $env:GESHER_WINDOWS_FONTS = NoDrive $fontLink
}
$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"
$env:GESHER_REQUIRE_BOOT_IMAGE = '1'
$bcp = (('core-oj', 'core-libart', 'core-icu4j') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$bcpSurfaces = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') |
  ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmSurfaces = @("-Xbootclasspath:$bcpSurfaces", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')

. (Join-Path $PSScriptRoot 'graphics/WindowChecks.ps1')

# Load the original capture function and its Win32 type, without executing
# unrelated track52 scenarios or copying a second pixel/process implementation.
$surfacePath = Join-Path $PSScriptRoot 'tracks/52-surfaces.ps1'
$surfaceTokens = $null
$surfaceErrors = $null
$surfaceAst = [Management.Automation.Language.Parser]::ParseFile(
  $surfacePath, [ref] $surfaceTokens, [ref] $surfaceErrors)
if ($surfaceErrors.Count) { throw 'Cannot parse the original Surface capture helper' }
$surfaceTypes = @($surfaceAst.EndBlock.Statements | Where-Object {
  $_.Extent.Text.StartsWith("if (-not ('GesherSurfWin32' -as [type]))")
})
$surfaceFunctions = @($surfaceAst.EndBlock.Statements | Where-Object {
  $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -eq 'SurfaceRun'
})
if ($surfaceTypes.Count -ne 1 -or $surfaceFunctions.Count -ne 1) {
  throw 'Expected exactly one original SurfaceRun function and Win32 helper type'
}
. ([scriptblock]::Create($surfaceTypes[0].Extent.Text))
. ([scriptblock]::Create($surfaceFunctions[0].Extent.Text))
Write-Host "INFO  component: original Surface capture helper SHA256 $((Get-FileHash -LiteralPath $surfacePath -Algorithm SHA256).Hash)"

foreach ($track in '80-surface-invalidation.ps1', '86-arm64-varargs.ps1') {
  $trackPath = Join-Path $PSScriptRoot "tracks/$track"
  if (-not (Test-Path -LiteralPath $trackPath)) { throw "Missing required original track: $track" }
  Write-Host "INFO  component: running exact $track SHA256 $((Get-FileHash -LiteralPath $trackPath -Algorithm SHA256).Hash)"
  . $trackPath
}
Write-Host "`n$failed test(s) failed"
exit $failed
