$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../tools/package/version.ps1')
$ordinary = Get-GesherPackageVersion '' '412'
if ($ordinary.Version -cne '0.1.412' -or $ordinary.Selection -cne 'ci-run-number') { throw 'Ordinary CI numbering changed' }
$release = Get-GesherPackageVersion '0.1.0' '412'
if ($release.Version -cne '0.1.0' -or $release.Selection -cne 'release') { throw 'Release override changed' }
foreach ($value in '0.1.1','v0.1.0','0.1.0 /DUnexpected=1',' 0.1.0') {
  $refused = $false
  try { $null = Get-GesherPackageVersion $value '412' } catch { $refused = $true }
  if (-not $refused) { throw "Accepted unexpected release selection '$value'" }
}
foreach ($value in '0','-1','65536','4;2') {
  $refused = $false
  try { $null = Get-GesherPackageVersion '' $value } catch { $refused = $true }
  if (-not $refused) { throw "Accepted invalid CI component '$value'" }
}
Write-Host 'PASS release version: ordinary CI numbering retained, exact 0.1.0 selected, malformed overrides refused'
