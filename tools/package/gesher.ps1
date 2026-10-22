# Runs a .dex program with Gesher's dalvikvm (ART on Windows, interpreter only).
#
#   gesher                              # the bundled Hello
#   gesher my.dex com.example.Main      # your own program
#   gesher my.dex com.example.Main arg1 arg2
#
# gesher.cmd runs this script. The .dex file must be on the same drive as
# Gesher: ART separates class path entries with ':', so paths are passed
# without the drive letter. The runtime's directories go to
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
foreach ($p in $dexFull, $root) {
  if ($p.Substring(0, 2) -ne $here.Substring(0, 2)) {
    throw "$p must be on drive $($here.Substring(0, 2)), with Gesher."
  }
}
# Drive-less paths resolve on the current drive.
Set-Location $here
function NoDrive([string] $p) { ($p -replace '\\', '/') -replace '^[A-Za-z]:', '' }

# The directories Android's runtime expects.
foreach ($d in 'system/framework', 'art', 'data', 'i18n/etc/icu', 'tzdata') {
  New-Item -ItemType Directory -Force (Join-Path $root $d) | Out-Null
}
# ICU's data, where the i18n module keeps it on Android; refreshed when a new
# version of Gesher brings a different one.
$data = Get-Item (Join-Path $here 'icudt*l.dat')
$icu = Join-Path $root "i18n/etc/icu/$($data.Name)"
if (-not (Test-Path $icu) -or (Get-Item $icu).Length -ne $data.Length) {
  Copy-Item $data.FullName $icu
}

$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"

$bcp = (('core-oj', 'core-libart', 'core-icu4j') | ForEach-Object { NoDrive "$here/$_.jar" }) -join ':'
& (Join-Path $here 'dalvikvm.exe') "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system/framework/boot.art")" `
  -Xint '-Xusejit:false' -cp (NoDrive $dexFull) $Class @ProgramArgs
exit $LASTEXITCODE
