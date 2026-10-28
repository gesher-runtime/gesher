# CI only: original AOT helpers and the complete current four-policy track73.
param([Parameter(Mandatory)][string] $Dir)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'CI only' }
$Dir = (Resolve-Path -LiteralPath $Dir).Path
$failed = 0
foreach ($name in 'dalvikvm.exe','gesher-exec.exe','gesher_arm64.dll','dex2oat.exe','profman.exe','dexdump.exe','oatdump.exe',
    'framework-res.apk','framework-res-after.apk','graphics-tests.dex','build.prop','hello.dex',
    'sharedlibraryapp.apk','sharedlibrarydlc.apk','sharedlibraryafter.apk','sharedlibraryafterdlc.apk',
    'sharedlibrarybad.apk','shared-base-v1.jar','shared-base-v2.jar','shared-entry.jar') {
  if (-not (Test-Path -LiteralPath (Join-Path $Dir $name) -PathType Leaf)) { throw "Missing current AOT member: $name" }
}
if ((Get-FileHash -LiteralPath "$Dir/framework-res.apk").Hash -eq
    (Get-FileHash -LiteralPath "$Dir/framework-res-after.apk").Hash) { throw 'Actual resource policy APK bytes did not change' }
foreach ($source in @(
    @{ Path='run-tests.ps1'; Names=@('NoDrive') },
    @{ Path='tracks/61-aot2.ps1'; Names=@('Aot2Run','Aot2Size','Aot2Methods','Aot2Read','Aot2Tail','Aot2Len') },
    @{ Path='tracks/63-aot3-cloud.ps1'; Names=@('Aot3Metadata') })) {
  $tokens = $null; $errors = $null
  $path = Join-Path $PSScriptRoot $source.Path
  $ast = [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
  if ($errors) { throw "Cannot parse unchanged helper source $path" }
  foreach ($name in $source.Names) {
    $definitions = @($ast.FindAll({ param($node)
      $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false))
    if ($definitions.Count -ne 1) { throw "Expected one original helper: $name" }
    . ([scriptblock]::Create($definitions[0].Extent.Text))
  }
  Write-Host "ACTUAL_AOT_HELPER path=$($source.Path) sha256=$((Get-FileHash -LiteralPath $path).Hash) source=$env:GITHUB_SHA"
}
$root = Join-Path $env:RUNNER_TEMP "aot3-resource-$([guid]::NewGuid().ToString('N'))"
if (Test-Path -LiteralPath $root) { throw 'Refusing to reuse a resource fixture root' }
foreach ($directory in 'system/framework/x86_64','system/fonts','system/etc','art','data','i18n/etc/icu','tzdata') {
  New-Item -ItemType Directory -Force (Join-Path $root $directory) | Out-Null
}
Copy-Item "$Dir/icudt*l.dat" (Join-Path $root 'i18n/etc/icu')
Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework')
Copy-Item "$Dir/build.prop" (Join-Path $root 'system')
Copy-Item "$Dir/fonts/*.ttf" (Join-Path $root 'system/fonts')
Copy-Item "$Dir/fonts/fonts.xml" (Join-Path $root 'system/etc/fonts.xml')
Copy-Item "$Dir/fonts/windows-fonts.xml" (Join-Path $root 'system/etc/windows-fonts.xml')
if (Test-Path -LiteralPath "$Dir/tzdata") { Copy-Item -Recurse -Force "$Dir/tzdata/*" (Join-Path $root 'tzdata') }
$fontLink = Join-Path $root 'windows-fonts'
New-Item -ItemType Junction -Path $fontLink -Target (Join-Path $env:WINDIR 'Fonts') | Out-Null
$env:GESHER_WINDOWS_FONTS = NoDrive $fontLink
foreach ($jar in 'core-oj','core-libart','core-icu4j','framework','gesher-app') {
  Write-Host "ACTUAL_AOT_JAR member=$jar.jar sha256=$((Get-FileHash -LiteralPath "$Dir/$jar.jar").Hash) source=$env:GITHUB_SHA"
}
foreach ($image in 'boot','boot-core-libart','boot-core-icu4j','boot-framework','boot-gesher-app') {
  foreach ($extension in 'art','oat','vdex') {
    $member = "$image.$extension"
    $from = Join-Path $Dir "framework/x86_64/$member"
    $to = Join-Path $root "system/framework/x86_64/$member"
    Copy-Item -LiteralPath $from -Destination $to
    $hash = (Get-FileHash -LiteralPath $from -Algorithm SHA256).Hash
    if ($hash -cne (Get-FileHash -LiteralPath $to -Algorithm SHA256).Hash) { throw "Image copy changed: $member" }
    Write-Host "ACTUAL_AOT_IMAGE member=$member sha256=$hash copied-identical=True source=$env:GITHUB_SHA"
  }
}
$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"
$env:GESHER_REQUIRE_BOOT_IMAGE = '1'
$aotRoot = Join-Path $root 'aot2'
New-Item -ItemType Directory -Force $aotRoot | Out-Null
$aotImage = "-Ximage:$(NoDrive "$root/system")/framework/boot.art"
$aotBcpApp = (('core-oj','core-libart','core-icu4j','framework','gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$aotLauncher = @("-Xbootclasspath:$aotBcpApp",$aotImage,'-Xint','-Xusejit:false','com.gesher.app.Launcher')
$resourcePath = Join-Path $root 'system/framework/framework-res.apk'
$originalResource = (Get-FileHash -LiteralPath $resourcePath).Hash
try {
  . "$PSScriptRoot/tracks/73-aot3-shared-libraries.ps1"
} finally {
  if ((Get-FileHash -LiteralPath $resourcePath).Hash -cne $originalResource) {
    Write-Host 'FAIL  after-resource component: final original resource hash differs'
    $failed++
  } else { Write-Host "ACTUAL_AOT_FINAL_RESOURCE_RESTORED sha256=$originalResource source=$env:GITHUB_SHA" }
}
Write-Host "$failed test(s) failed"
if ($failed) { exit 1 }
