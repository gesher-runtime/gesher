param(
  [Parameter(Mandatory)][string] $Installer,
  [Parameter(Mandatory)][string] $Output,
  [Parameter(Mandatory)][string] $Version,
  [Parameter(Mandatory)][ValidateSet('release','ci-run-number')][string] $Selection,
  [Parameter(Mandatory)][long] $ArtifactId,
  [Parameter(Mandatory)][string] $ArtifactDigest
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^0\.1\.(0|[1-9][0-9]*)$') { throw 'Unexpected package version' }
if (($Selection -eq 'release') -ne ($Version -eq '0.1.0')) { throw 'Release version selection mismatch' }
if ($ArtifactId -le 0 -or $ArtifactDigest -notmatch '^(sha256:)?[a-fA-F0-9]{64}$') { throw 'Missing installer artifact identity' }
$file = Get-Item -LiteralPath $Installer
if ($file.Name -cne "gesher-setup-$Version.exe") { throw 'Installer filename/version mismatch' }
function Check-Version([string] $Value) {
  if ($Value -notmatch '^([0-9]+)\.([0-9]+)\.([0-9]+)(?:\.0)?$' -or
      "$($Matches[1]).$($Matches[2]).$($Matches[3])" -cne $Version) { throw "PE version '$Value' does not match $Version" }
}
$info = [Diagnostics.FileVersionInfo]::GetVersionInfo($file.FullName)
Check-Version $info.ProductVersion
Check-Version $info.FileVersion
if ($info.ProductName -cne 'Gesher') { throw "Unexpected PE product '$($info.ProductName)'" }
foreach ($key in 'GITHUB_REPOSITORY','GITHUB_REF','GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT','GITHUB_EVENT_NAME') {
  if (-not [Environment]::GetEnvironmentVariable($key)) { throw "Missing provenance $key" }
}
if ($env:GITHUB_SHA -notmatch '^[a-fA-F0-9]{40}$' -or $env:GITHUB_RUN_ID -notmatch '^[1-9][0-9]*$' -or $env:GITHUB_RUN_ATTEMPT -notmatch '^[1-9][0-9]*$') { throw 'Invalid CI provenance' }
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLowerInvariant()
$manifest = [ordered] @{
  schema = 1; product = 'Gesher'; version = $Version; version_selection = $Selection
  installer = [ordered] @{ name = $file.Name; size = $file.Length; sha256 = $hash
    product_name = $info.ProductName; product_version = $info.ProductVersion; file_version = $info.FileVersion }
  source = [ordered] @{ repository = $env:GITHUB_REPOSITORY; ref = $env:GITHUB_REF
    sha = $env:GITHUB_SHA.ToLowerInvariant(); workflow = '.github/workflows/ci.yml'
    event = $env:GITHUB_EVENT_NAME; run_id = [long]$env:GITHUB_RUN_ID; run_attempt = [int]$env:GITHUB_RUN_ATTEMPT }
  artifact = [ordered] @{ name = 'gesher-setup'; id = $ArtifactId
    digest = 'sha256:' + ($ArtifactDigest -replace '^sha256:', '').ToLowerInvariant() }
  # This script is called only after the package job's complete smoke step.
  package_checks = @('installed_boot_image','hello','framework_lifecycle','device_apps','window_input','uninstall_cleanup')
}
$directory = [IO.Path]::GetFullPath($Output)
[void][IO.Directory]::CreateDirectory($directory)
$manifestName = "gesher-$Version-manifest.json"
$encoding = [Text.UTF8Encoding]::new($false)
$manifestPath = Join-Path $directory $manifestName
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 6) + "`n", $encoding)
$manifestHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $manifestPath).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $directory 'SHA256SUMS'), "$hash  $($file.Name)`n$manifestHash  $manifestName`n", $encoding)
Write-Host "PASS release receipt: $($file.Name) $Version SHA256 $hash after package install/run/uninstall checks"
