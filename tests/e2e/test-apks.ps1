# Exercise transport/cache failures with tiny own fixture bytes, no APK download.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'apks.ps1')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('gesher-apk-test-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $fixture | Out-Null
$payload = [Text.Encoding]::UTF8.GetBytes('Gesher pinned transport fixture')
$pin = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($payload))
$path = Join-Path $fixture 'fixture.apk'
$script:calls = 0
$script:mode = 'fallback'
$failed = 0

function Invoke-WebRequest($Uri, $OutFile, [switch] $UseBasicParsing, $TimeoutSec, $MaximumRetryCount, $RetryIntervalSec) {
  $script:calls++
  if ($script:mode -eq 'fallback' -and $Uri.StartsWith('https://primary.invalid/')) {
    throw [Net.WebException]::new('No such host is known')
  }
  $bytes = if ($script:mode -eq 'corrupt') { [byte[]] @(1, 2, 3) } else { $payload }
  [IO.File]::WriteAllBytes($OutFile, $bytes)
}

function ApkCheck([string] $Name, [bool] $Good) {
  if ($Good) { Write-Host "PASS  e2e APK transport: $Name" }
  else { Write-Host "FAIL  e2e APK transport: $Name"; $script:failed++ }
}

try {
  $repositories = @('https://primary.invalid/repo', 'https://mirror.invalid/repo')
  Get-PinnedApk $path 'fixture.apk' $pin $repositories
  ApkCheck 'DNS failure falls back to a mirror with the exact pinned bytes' ($script:calls -eq 2 -and (Get-FileHash -LiteralPath $path).Hash -eq $pin)
  $script:calls = 0
  Get-PinnedApk $path 'fixture.apk' $pin $repositories
  ApkCheck 'valid cache requires no network request' ($script:calls -eq 0)

  [IO.File]::WriteAllBytes($path, [byte[]] @(9, 8, 7))
  $script:mode = 'valid'
  Get-PinnedApk $path 'fixture.apk' $pin $repositories
  ApkCheck 'corrupt cached bytes are replaced only by pinned bytes' ($script:calls -eq 1 -and (Get-FileHash -LiteralPath $path).Hash -eq $pin)

  Remove-Item -LiteralPath $path
  $script:calls = 0
  $script:mode = 'corrupt'
  $rejected = $false
  try { Get-PinnedApk $path 'fixture.apk' $pin $repositories } catch { $rejected = $_ -match 'SHA-256' }
  ApkCheck 'all mismatched downloads fail and leave no reusable partial file' ($rejected -and $script:calls -eq 2 -and -not (Test-Path -LiteralPath $path) -and -not (Test-Path -LiteralPath "$path.partial"))
} finally {
  foreach ($file in @($path, "$path.partial")) {
    if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file }
  }
  Remove-Item -LiteralPath $fixture
}
Write-Host "$failed test(s) failed"
if ($failed) { exit 1 }
