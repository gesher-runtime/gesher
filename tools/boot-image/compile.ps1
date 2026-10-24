# Compiles Gesher's boot image with dex2oat.exe (docs/boot-image.md, ADR 0031):
#
#   pwsh tools/boot-image/compile.ps1 -Dist <dir> -Out <dir>
#
# <dir> holds dex2oat.exe and the boot class path's jars (core-oj, core-libart,
# core-icu4j, framework, gesher-app). Writes <Out>/framework/x86_64/:
#   boot.art/.oat/.vdex, boot-core-libart.*, boot-core-icu4j.*  the primary image
#   boot-framework.*                                           framework.jar's extension
#   boot-gesher-app.*                                          gesher-app.jar's extension
# The image has no compiled code (--compiler-filter=verify): the runtime runs
# with the interpreter, and the image holds the boot class path's classes,
# verified, linked and, where dex2oat could, initialized, with their strings.
# Dex locations are bare file names, so the image loads wherever the jars are.
param(
  [Parameter(Mandatory)] [string] $Dist,
  [Parameter(Mandatory)] [string] $Out
)
$ErrorActionPreference = 'Stop'

function NoDrive([string] $p) { ($p -replace '\\', '/') -replace '^[A-Za-z]:', '' }

$dist = (Resolve-Path $Dist).Path
New-Item -ItemType Directory -Force (Join-Path $Out 'framework/x86_64') | Out-Null
$outDir = (Resolve-Path $Out).Path
if ($outDir.Substring(0, 2) -ne $dist.Substring(0, 2)) { throw "$Out must be on the drive of $Dist" }
# ART's paths go without the drive letter (':' separates class path entries).
Set-Location $dist
$image = NoDrive "$outDir/framework"
$isa = "$image/x86_64"

# What dex2oat's runtime expects of its environment.
$root = Join-Path $outDir 'root'
foreach ($d in 'system/framework', 'art', 'data/dalvik-cache', 'i18n', 'tzdata') {
  New-Item -ItemType Directory -Force (Join-Path $root $d) | Out-Null
}
$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"

$jars = 'core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app'
$common = @(
  '--runtime-arg', '-Xms64m', '--runtime-arg', '-Xmx2g',
  "--android-root=$(NoDrive "$root/system")",
  '--instruction-set=x86_64', '--compiler-filter=verify', '--image-format=lz4',
  '--no-generate-mini-debug-info', '--no-watch-dog', '--avoid-storing-invocation', '-j4')

# One image chunk: `count` jars from `first`, on top of the images named in $boot.
function Compile([int] $first, [int] $count, [string] $boot) {
  $bcp = $jars[0..($first + $count - 1)]
  $a = $common + @(
    "--runtime-arg", "-Xbootclasspath:$(($bcp | ForEach-Object { NoDrive "$dist/$_.jar" }) -join ':')",
    "--runtime-arg", "-Xbootclasspath-locations:$(($bcp | ForEach-Object { "$_.jar" }) -join ':')",
    "--image=$isa/boot.art", "--oat-file=$isa/boot.oat")
  foreach ($jar in $jars[$first..($first + $count - 1)]) {
    $a += "--dex-file=$(NoDrive "$dist/$jar.jar")", "--dex-location=$jar.jar"
  }
  if ($boot) { $a += "--boot-image=$boot" } else { $a += '--base=0x60000000' }
  $what = ($jars[$first..($first + $count - 1)] -join ', ')
  Write-Host "dex2oat: $what"
  $watch = [Diagnostics.Stopwatch]::StartNew()
  & (Join-Path $dist 'dex2oat.exe') @a 2>&1 | Out-Host
  if ($LASTEXITCODE -ne 0) { throw ('dex2oat failed for {0} (exit 0x{1:X8})' -f $what, $LASTEXITCODE) }
  Write-Host ("dex2oat: {0} in {1:N1} s" -f $what, $watch.Elapsed.TotalSeconds)
}

Compile 0 3 ''
Compile 3 1 "$image/boot.art"
Compile 4 1 "$image/boot.art:$image/boot-framework.art"

Remove-Item -Recurse -Force $root
Get-ChildItem (Join-Path $outDir 'framework/x86_64') | Format-Table Name, Length | Out-Host
