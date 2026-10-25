# Compiles Gesher's boot image with dex2oat.exe (docs/boot-image.md, ADR 0031):
#
#   pwsh tools/boot-image/compile.ps1 -Dist <dir> -Out <dir>
#
# <dir> holds dex2oat.exe and the boot class path's jars (core-oj, core-libart,
# core-icu4j, framework, gesher-app). Writes <Out>/framework/x86_64/:
#   boot.art/.oat/.vdex, boot-core-libart.*, boot-core-icu4j.*  the primary image
#   boot-framework.*                                           framework.jar's extension
#   boot-gesher-app.*                                          gesher-app.jar's extension
# -Filter is the primary image's compiler filter, -ExtensionFilter the
# extensions': `speed` compiles every method with the optimizing compiler (AOT
# code, docs/compiled-code.md); `verify` compiles none, the image then holds
# the boot class path's classes, verified, linked and, where dex2oat could,
# initialized, with their strings, and the runtime runs them with nterp.
# Dex locations are bare file names, so the image loads wherever the jars are.
#
# -BootProfiles names boot image profiles in text form (AOSP's
# art/build/boot/boot-image-profile.txt and frameworks/base/boot/
# boot-image-profile.txt, fetched by tools/boot-image/fetch-profiles.ps1):
# profman.exe turns them into one binary boot profile (as Soong's
# boot image profile rule does), and `speed-profile` filters compile only the
# methods it marks and put only the classes it lists in the image, as
# Android's boot image is made (docs/compiled-code.md, ADR 0054).
# -AppFilter is gesher-app.jar's filter (no AOSP profile covers it).
#
# BOOT_IMAGE_VERBOSE=compiler (or other -verbose: tags) logs dex2oat's phases.
param(
  [Parameter(Mandatory)] [string] $Dist,
  [Parameter(Mandatory)] [string] $Out,
  [string] $Filter = 'verify',
  [string] $ExtensionFilter = 'verify',
  [string] $AppFilter = '',
  [string[]] $BootProfiles = @(),
  [int] $TimeoutMinutes = 20
)
$ErrorActionPreference = 'Stop'
# pwsh -File passes "a,b" as one string.
$BootProfiles = @($BootProfiles | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | ForEach-Object { (Resolve-Path $_).Path })

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
  # A large object space of its own map per object: the free-list one is one
  # reservation of the heap's capacity, which ART's low-4 GiB allocator looks
  # for page by page (msync), slow on Windows for large sizes.
  '--runtime-arg', '-Xms64m', '--runtime-arg', '-Xmx512m', '--runtime-arg', '-XX:LargeObjectSpace=map',
  "--android-root=$(NoDrive "$root/system")",
  '--instruction-set=x86_64', '--image-format=lz4',
  '--no-generate-mini-debug-info', '--no-watch-dog', '--avoid-storing-invocation', '-j4')

# The boot image profile: the text profiles, concatenated, in binary form for
# the boot class path's jars (profman --output-profile-type=boot, as Soong's
# boot image profile rule runs it), with the image's dex locations.
$bootProfile = $null
if ($BootProfiles) {
  $text = Join-Path $root 'boot-image-profile.txt'
  $lines = 0
  Set-Content -NoNewline -Path $text -Value ''
  foreach ($p in $BootProfiles) {
    $content = [IO.File]::ReadAllText((Resolve-Path $p).Path)
    if (-not $content.EndsWith("`n")) { $content += "`n" }
    [IO.File]::AppendAllText($text, $content)
    $lines += ($content -split "`n").Count - 1
  }
  $bootProfile = NoDrive (Join-Path $root 'boot.prof')
  $pa = @('--output-profile-type=boot', "--create-profile-from=$(NoDrive $text)", "--reference-profile-file=$bootProfile")
  foreach ($jar in $jars) { $pa += "--apk=$(NoDrive "$dist/$jar.jar")", "--dex-location=$jar.jar" }
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $out = & (Join-Path $dist 'profman.exe') @pa 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $root 'boot.prof'))) {
    Write-Host $out
    throw ('profman failed to make the boot image profile (exit 0x{0:X8})' -f $LASTEXITCODE)
  }
  Write-Host ("profman: boot image profile from {0} ({1:N0} lines): {2:N0} bytes in {3:N1} s" -f
    (($BootProfiles | ForEach-Object { Split-Path -Leaf $_ }) -join ' + '), $lines, (Get-Item (Join-Path $root 'boot.prof')).Length, $watch.Elapsed.TotalSeconds)
}

# One image chunk: `count` jars from `first`, on top of the images named in $boot.
function Compile([int] $first, [int] $count, [string] $boot, [string] $filter) {
  $bcp = $jars[0..($first + $count - 1)]
  $a = $common + @(
    "--runtime-arg", "-Xbootclasspath:$(($bcp | ForEach-Object { NoDrive "$dist/$_.jar" }) -join ':')",
    "--runtime-arg", "-Xbootclasspath-locations:$(($bcp | ForEach-Object { "$_.jar" }) -join ':')",
    "--image=$isa/boot.art", "--oat-file=$isa/boot.oat")
  foreach ($jar in $jars[$first..($first + $count - 1)]) {
    $a += "--dex-file=$(NoDrive "$dist/$jar.jar")", "--dex-location=$jar.jar"
  }
  if ($boot) { $a += "--boot-image=$boot" } else { $a += '--base=0x60000000' }
  $a += "--compiler-filter=$filter"
  if ($filter -eq 'speed-profile') {
    if (-not $bootProfile) { throw "the speed-profile filter needs -BootProfiles" }
    $a += "--profile-file=$bootProfile"
  }
  if ($env:BOOT_IMAGE_VERBOSE) { $a += '--runtime-arg', "-verbose:$env:BOOT_IMAGE_VERBOSE", '--dump-timings' }
  $what = "$($jars[$first..($first + $count - 1)] -join ', ') ($filter)"
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

Compile 0 3 '' $Filter
Compile 3 1 "$image/boot.art" $ExtensionFilter
Compile 4 1 "$image/boot.art:$image/boot-framework.art" $(if ($AppFilter) { $AppFilter } else { $ExtensionFilter })

Remove-Item -Recurse -Force $root
$files = Get-ChildItem (Join-Path $outDir 'framework/x86_64')
$files | Format-Table Name, Length | Out-Host
foreach ($ext in 'art', 'oat', 'vdex') {
  Write-Host ("boot image: .{0} files {1:N1} MB" -f $ext, (($files | Where-Object Extension -eq ".$ext" | Measure-Object Length -Sum).Sum / 1MB))
}
Write-Host ("boot image: {0:N1} MB in all" -f (($files | Measure-Object Length -Sum).Sum / 1MB))
