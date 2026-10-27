# CI only: unchanged accepted ART/Java/boot-image bytes, scoped real test54.
param([Parameter(Mandatory)][string] $Dir, [Parameter(Mandatory)][string] $Observer)
$ErrorActionPreference = 'Stop'
$failed = 0
$Dir = (Resolve-Path -LiteralPath $Dir).Path
$script:web3Observer = (Resolve-Path -LiteralPath $Observer).Path
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
  (Join-Path $PSScriptRoot 'run-tests.ps1'), [ref]$tokens, [ref]$errors)
if ($errors) { throw 'Cannot parse actual NoDrive helper source' }
$definitions = @($ast.FindAll({param($node)
  $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'NoDrive'
}, $false))
if ($definitions.Count -ne 1) { throw 'Expected exactly one actual NoDrive helper' }
. ([scriptblock]::Create($definitions[0].Extent.Text))
. "$PSScriptRoot/web3/Observer.ps1"
Web3ObserverControl

$root = Join-Path $env:RUNNER_TEMP "web3-probe-$env:GITHUB_RUN_ID"
if (Test-Path -LiteralPath $root) { throw 'Refusing to reuse an existing diagnostic root' }
foreach ($directory in 'system/framework/x86_64','system/fonts','system/etc','art','data','i18n/etc/icu','tzdata') {
  New-Item -ItemType Directory -Force (Join-Path $root $directory) | Out-Null
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
New-Item -ItemType Junction -Path $fontLink -Target (Join-Path $env:WINDIR 'Fonts') | Out-Null
$env:GESHER_WINDOWS_FONTS = NoDrive $fontLink
$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"
$env:GESHER_REQUIRE_BOOT_IMAGE = '1'
Write-Host "WEB3_DIAGNOSTIC_ONLY accepted-runtime=$env:SOURCE_RUN/$env:SOURCE_SHA harness=$env:GITHUB_SHA no-production-rebuild=true"
. "$PSScriptRoot/tracks/54-web3.ps1"
Write-Host "$failed test(s) failed"
exit $(if ($failed) { 1 } else { 0 })
