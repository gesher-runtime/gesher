# CI-only diagnostic: the unchanged real test73 against explicitly pinned build artifacts.
param([Parameter(Mandatory)][string] $Dir)
$ErrorActionPreference = 'Stop'
$failed = 0
$Dir = (Resolve-Path -LiteralPath $Dir).Path
foreach ($source in @(
    @{ Path='run-tests.ps1'; Names=@('NoDrive') },
    @{ Path='tracks/61-aot2.ps1'; Names=@('Aot2Run','Aot2Size','Aot2Methods','Aot2Read','Aot2Tail','Aot2Len') },
    @{ Path='tracks/63-aot3-cloud.ps1'; Names=@('Aot3Metadata') })) {
  $tokens = $null; $parseErrors = $null
  $ast = [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot $source.Path), [ref]$tokens, [ref]$parseErrors)
  if ($parseErrors) { throw "Cannot parse actual helper source $($source.Path)" }
  foreach ($name in $source.Names) {
    $definitions = @($ast.FindAll({ param($node)
      $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false))
    if ($definitions.Count -ne 1) { throw "Expected one actual helper $name" }
    . ([scriptblock]::Create($definitions[0].Extent.Text))
  }
}
$root = Join-Path $env:RUNNER_TEMP "aot3-shared-probe-$env:GITHUB_RUN_ID"
if (Test-Path -LiteralPath $root) { throw 'Refusing to reuse an existing probe root' }
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
$aotRoot = Join-Path $root 'aot2'
New-Item -ItemType Directory -Force $aotRoot | Out-Null
$aotImage = "-Ximage:$(NoDrive "$root/system/framework/boot.art")"
$aotBcpApp = (('core-oj','core-libart','core-icu4j','framework','gesher-app') |
  ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$aotLauncher = @("-Xbootclasspath:$aotBcpApp",$aotImage,'-Xint','-Xusejit:false','com.gesher.app.Launcher')
Write-Host "AOT_SHARED_DIAGNOSTIC_ONLY runtime=$env:SOURCE_RUN/$env:SOURCE_SHA Java=$env:FRAMEWORK_RUN/$env:FRAMEWORK_SHA fresh-image=true harness=$env:GITHUB_SHA"
. "$PSScriptRoot/tracks/73-aot3-shared-libraries.ps1"
Write-Host "$failed test(s) failed"
if ($failed) { exit 1 }
