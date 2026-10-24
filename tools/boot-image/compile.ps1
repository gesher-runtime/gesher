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
#
# BOOT_IMAGE_VERBOSE=compiler (or other -verbose: tags) logs dex2oat's phases.
param(
  [Parameter(Mandatory)] [string] $Dist,
  [Parameter(Mandatory)] [string] $Out,
  [int] $TimeoutMinutes = 20
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
  '--runtime-arg', '-Xms64m', '--runtime-arg', '-Xmx1g',
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
  if ($env:BOOT_IMAGE_VERBOSE) { $a += '--runtime-arg', "-verbose:$env:BOOT_IMAGE_VERBOSE", '--dump-timings' }
  $what = ($jars[$first..($first + $count - 1)] -join ', ')
  Write-Host "dex2oat: $what"
  $log = Join-Path $root "dex2oat-$first.log"
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process (Join-Path $dist 'dex2oat.exe') -ArgumentList $a -NoNewWindow -PassThru `
    -RedirectStandardOutput "$log.out" -RedirectStandardError $log
  $null = $p.Handle  # keeps the exit code (Start-Process drops it otherwise)
  $done = $p.WaitForExit($TimeoutMinutes * 60 * 1000)
  if (-not $done) {
    Write-Host "dex2oat: $what still runs after $TimeoutMinutes minutes; its threads:"
    Stacks $p.Id
    Stop-Process -Id $p.Id -Force
  }
  $p.WaitForExit()
  $code = $p.ExitCode
  $out = @(Get-Content "$log.out", $log -ErrorAction SilentlyContinue)
  # The phases (-verbose:compiler), then the end of what it printed.
  $out | Select-String -Pattern 'dex2oat.cc|compiler_driver.cc.*(Before precompile|LoadImageClasses:|Resolve:|Verify:|InitializeClasses:|UpdateImageClasses:)|image_writer|Error|error|FATAL|Abort' |
    Select-Object -First 80 | Out-Host
  Write-Host "... ($($out.Count) lines; the last 60:)"
  $out | Select-Object -Last 60 | Out-Host
  if (-not $done -or $code -ne 0) { throw ('dex2oat failed for {0} (exit 0x{1:X8})' -f $what, $code) }
  Write-Host ("dex2oat: {0} in {1:N1} s" -f $what, $watch.Elapsed.TotalSeconds)
}

# Every thread's stack of a process that hangs, with cdb if the machine has it,
# symbolized with dex2oat.map.
function Stacks([int] $id) {
  $cdb = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\cdb.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1
  if (-not $cdb) { Write-Host '(no cdb.exe on this machine)'; return }
  $text = & $cdb.FullName -p $id -c '.symopt+0x40; ~*k 30; qd' 2>&1 | Out-String
  $text = $text -replace 'dex2oat\+0x', 'dex2oat.exe+0x'
  $map = Join-Path $dist 'dex2oat.map'
  if ((Test-Path $map) -and (Get-Command python -ErrorAction SilentlyContinue)) {
    $text | python (Join-Path $PSScriptRoot '../symbolize.py') $map | Out-Host
  } else {
    Write-Host $text
  }
}

Compile 0 3 ''
Compile 3 1 "$image/boot.art"
Compile 4 1 "$image/boot.art:$image/boot-framework.art"

Remove-Item -Recurse -Force $root
Get-ChildItem (Join-Path $outDir 'framework/x86_64') | Format-Table Name, Length | Out-Host
