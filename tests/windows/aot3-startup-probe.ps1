# Run the real startup-profile collector against an explicitly selected CI build.
param([Parameter(Mandatory)] [string] $Dir)
$ErrorActionPreference = 'Stop'
$failed = 0
$Dir = (Resolve-Path -LiteralPath $Dir).Path

# Reuse the suite's actual helpers without running its unrelated test bodies.
foreach ($source in @(
    @{ Path = 'run-tests.ps1'; Names = @('NoDrive') },
    @{ Path = 'tracks/61-aot2.ps1'; Names = @('Aot2Run', 'Aot2Size') },
    @{ Path = 'tracks/62-aot3.ps1'; Names = @('Aot3Check') })) {
  $tokens = $null; $errors = $null
  $ast = [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot $source.Path), [ref]$tokens, [ref]$errors)
  if ($errors) { throw "Cannot parse helper source $($source.Path): $errors" }
  foreach ($name in $source.Names) {
    $definitions = @($ast.FindAll({ param($node)
      $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false))
    if ($definitions.Count -ne 1) { throw "Expected one helper $name in $($source.Path)" }
    . ([scriptblock]::Create($definitions[0].Extent.Text))
  }
}

$root = Join-Path $env:RUNNER_TEMP 'aot3-startup-probe'
foreach ($dirName in 'system/framework/x86_64', 'system/fonts', 'system/etc', 'art', 'data', 'i18n/etc/icu', 'tzdata') {
  New-Item -ItemType Directory -Force (Join-Path $root $dirName) | Out-Null
}
Copy-Item "$Dir/icudt*l.dat" (Join-Path $root 'i18n/etc/icu')
if (Test-Path "$Dir/tzdata") { Copy-Item -Recurse -Force "$Dir/tzdata/*" (Join-Path $root 'tzdata') }
Copy-Item "$Dir/framework/x86_64/*" (Join-Path $root 'system/framework/x86_64')
Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework')
Copy-Item "$Dir/build.prop" (Join-Path $root 'system')
Copy-Item "$Dir/fonts/*.ttf" (Join-Path $root 'system/fonts')
Copy-Item "$Dir/fonts/fonts.xml" (Join-Path $root 'system/etc/fonts.xml')
Copy-Item "$Dir/fonts/windows-fonts.xml" (Join-Path $root 'system/etc/windows-fonts.xml')
$fontLink = Join-Path $root 'windows-fonts'
if (-not (Test-Path $fontLink)) {
  New-Item -ItemType Junction -Path $fontLink -Target (Join-Path $env:WINDIR 'Fonts') | Out-Null
}
$env:GESHER_WINDOWS_FONTS = NoDrive $fontLink
$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"
$env:GESHER_REQUIRE_BOOT_IMAGE = '1'
$aotRoot = Join-Path $root 'aot2'
$aotImage = "-Ximage:$(NoDrive "$root/system")/framework/boot.art"
$aotBcpApp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') |
  ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
. "$PSScriptRoot/tracks/64-aot3-boot-profile.ps1"
Write-Host "$failed test(s) failed"
if ($failed) { exit 1 }
