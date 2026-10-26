# Keep ordinary CI numbering; a release version is always explicit.
function Get-GesherPackageVersion([string] $Requested, [string] $RunNumber) {
  if ($Requested) {
    if ($Requested -cne '0.1.0') { throw 'The supported release version is exactly 0.1.0' }
    return [pscustomobject] @{ Version = '0.1.0'; Selection = 'release' }
  }
  if ($RunNumber -notmatch '^[1-9][0-9]*$' -or [long]$RunNumber -gt 65535) {
    throw 'CI run number must fit an Inno Setup version component (1..65535)'
  }
  return [pscustomobject] @{ Version = "0.1.$RunNumber"; Selection = 'ci-run-number' }
}
