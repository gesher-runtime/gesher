# Fetches AOSP's boot image profiles in text form, at aosp.lock's tag, for
# tools/boot-image/compile.ps1 -Profile (docs/compiled-code.md, ADR 0054):
#
#   pwsh tools/boot-image/fetch-profiles.ps1 -Out <dir>
#
# writes <dir>/art-boot-image-profile.txt (art/build/boot: core-oj,
# core-libart, core-icu4j and the ART module's other jars) and
# <dir>/framework-boot-image-profile.txt (frameworks/base/boot: framework.jar
# and the platform's other boot jars), as Soong combines them for the boot
# image. Gitiles serves a file base64-encoded with ?format=TEXT.
param(
  [Parameter(Mandatory)] [string] $Out,
  [string] $Lock = (Join-Path $PSScriptRoot '../../aosp.lock')
)
$ErrorActionPreference = 'Stop'

$tag = (Get-Content $Lock | Where-Object { $_ -match '^tag\s+(\S+)' } | Select-Object -First 1) -replace '^tag\s+', ''
if (-not $tag) { throw "no tag in $Lock" }
New-Item -ItemType Directory -Force $Out | Out-Null
$files = [ordered]@{
  'art-boot-image-profile.txt' = 'platform/art/+/refs/tags/{0}/build/boot/boot-image-profile.txt'
  'framework-boot-image-profile.txt' = 'platform/frameworks/base/+/refs/tags/{0}/boot/boot-image-profile.txt'
}
foreach ($name in $files.Keys) {
  $url = 'https://android.googlesource.com/' + ($files[$name] -f $tag) + '?format=TEXT'
  $dest = Join-Path $Out $name
  for ($try = 1; ; $try++) {
    try {
      $b64 = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 120 $url).Content
      if ($b64 -is [byte[]]) { $b64 = [Text.Encoding]::ASCII.GetString($b64) }
      [IO.File]::WriteAllBytes($dest, [Convert]::FromBase64String($b64))
      break
    } catch {
      if ($try -ge 5) { throw "fetching $url failed: $_" }
      Write-Host "fetching $url failed ($_); again in $(10 * $try) s"
      Start-Sleep -Seconds (10 * $try)
    }
  }
  $lines = (Get-Content $dest | Measure-Object -Line).Lines
  Write-Host ("{0}: {1:N0} lines, {2:N0} bytes ({3})" -f $name, $lines, (Get-Item $dest).Length, $tag)
}
