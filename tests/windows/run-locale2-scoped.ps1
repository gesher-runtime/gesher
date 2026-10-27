# CI-only actual ART/APK execution. No boot-image or installer/e2e acceptance.
param([Parameter(Mandatory)] [string] $Dir)
$ErrorActionPreference = 'Stop'
$failed = 0
function NoDrive([string] $Path) { ($Path -replace '\\', '/') -replace '^[A-Za-z]:', '' }
$root = Join-Path $env:RUNNER_TEMP 'gesher-locale2-scoped'
foreach ($dirName in 'system/framework', 'system/fonts', 'system/etc', 'art', 'data', 'i18n/etc/icu', 'tzdata') {
  New-Item -ItemType Directory -Force (Join-Path $root $dirName) | Out-Null
}
Copy-Item "$Dir/icudt*l.dat" (Join-Path $root 'i18n/etc/icu')
Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework')
Copy-Item "$Dir/build.prop" (Join-Path $root 'system')
Copy-Item "$Dir/fonts/*.ttf" (Join-Path $root 'system/fonts')
Copy-Item "$Dir/fonts/fonts.xml" (Join-Path $root 'system/etc')
if (Test-Path "$Dir/tzdata") { Copy-Item -Recurse -Force "$Dir/tzdata/*" (Join-Path $root 'tzdata') }
$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"
$env:GESHER_TRACE_FAULTS = '1'
Remove-Item Env:GESHER_REQUIRE_BOOT_IMAGE -ErrorAction SilentlyContinue
$bcpWindows = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') |
  ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmWindows = @("-Xbootclasspath:$bcpWindows", "-Ximage:$(NoDrive $root)/absent.art", '-Xint', '-Xusejit:false')

# Use the existing real process runner and report-field parser from test49,
# without running its NLS/legacy test body or compiling its unrelated C# helper.
$errors = $null
$tokens = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
  (Join-Path $PSScriptRoot 'tracks/49-locale.ps1'), [ref] $tokens, [ref] $errors)
if ($errors.Count) { throw $errors }
$names = @('LocaleRun', 'LocaleStart', 'LocaleWait', 'LocaleField')
$functions = @($ast.FindAll({ param($node)
  $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -in $names
}, $true))
if ($functions.Count -ne $names.Count) { throw 'Actual locale test runner helpers are missing' }
foreach ($function in $functions) { Invoke-Expression $function.Extent.Text }

Write-Host 'INFO  locale2: current compiled Java/APKs; exact accepted native runtime; imageless scoped execution'
. "$PSScriptRoot/tracks/81-locale2.ps1"
Write-Host "$failed test(s) failed"
if ($failed) { exit 1 }
