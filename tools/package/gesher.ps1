# Runs a .dex program with Gesher's dalvikvm (ART on Windows, interpreter only).
#
#   gesher                              # the bundled Hello
#   gesher my.dex com.example.Main      # your own program
#   gesher my.dex com.example.Main arg1 arg2
#
# gesher.cmd runs this script. ART separates class path entries with ':', so
# paths are passed without the drive letter (another drive is /mnt/<drive>). The runtime's directories go to
# %LOCALAPPDATA%\Gesher, as the install folder may not be writable.
param(
  [string] $Dex = '',
  [string] $Class = 'gesher.Hello',
  [Parameter(ValueFromRemainingArguments)] [string[]] $ProgramArgs = @()
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if ($Dex -eq '') { $Dex = Join-Path $here 'hello.dex' }  # PowerShell 5: no $PSScriptRoot in param defaults

$dexFull = (Resolve-Path $Dex).Path
$root = Join-Path $env:LOCALAPPDATA 'Gesher'
. (Join-Path $here 'gesher-runtime-files.ps1')
# Drive-less paths resolve on the current drive.
Set-Location -LiteralPath $here
# A Windows path as gesher-base's path model reads it (docs/posix.md): no drive
# on the current drive, /mnt/<drive>/... on another, so ':'-separated lists
# hold no drive colons.
function NoDrive([string] $p) {
  return ConvertTo-GesherAndroidPath -Path $p
}
# The user's own dex path keeps its long form: only Gesher's roots take
# 8.3 short paths, so file names and extensions stay as given (docs/installer.md).
function UserPath([string] $p) {
  return ConvertTo-GesherAndroidPath -Path $p -KeepLongPath
}

# The directories Android's runtime expects.
foreach ($d in 'system/framework', 'art', 'data', 'i18n/etc/icu', 'tzdata') {
  New-Item -ItemType Directory -Force (Join-Path $root $d) | Out-Null
}
# ICU's data, where the i18n module keeps it on Android; refreshed when a new
# version of Gesher brings a different one.
$runtimeSync = New-GesherRuntimeSync -InstallDirectory $here -ProfileDirectory $root -Component dex
$data = Get-Item (Join-Path $here 'icudt*l.dat')
$icu = Join-Path $root "i18n/etc/icu/$($data.Name)"
Sync-GesherRuntimeFile -Source $data.FullName -Destination $icu -State $runtimeSync
Complete-GesherRuntimeSync -State $runtimeSync

$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
# Time zone data (docs/posix.md), read in place from the install folder.
$env:ANDROID_TZDATA_ROOT = NoDrive "$here/tzdata"

$bcp = (('core-oj', 'core-libart', 'core-icu4j') | ForEach-Object { NoDrive "$here/$_.jar" }) -join ':'
# The boot image (docs/boot-image.md) is installed next to the jars, in framework\x86_64.
& (Join-Path $here 'dalvikvm.exe') "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$here/framework/boot.art")" `
  -Xint '-Xusejit:false' -cp (UserPath $dexFull) $Class @ProgramArgs
exit $LASTEXITCODE
