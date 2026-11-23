# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Explicit operator decisions for isolated APK tests. RunTimed owns the child
# process/readers; identity overrides never remain in the invoking test process.
function Invoke-FixtureProcess([string] $Exe, [string[]] $Arguments,
    [int] $Seconds = 120, [hashtable] $Environment = @{}) {
  $saved = @{}
  try {
    foreach ($entry in $Environment.GetEnumerator()) {
      $saved[$entry.Key] = [Environment]::GetEnvironmentVariable($entry.Key)
      [Environment]::SetEnvironmentVariable($entry.Key, [string]$entry.Value)
    }
    RunTimed $Exe $Arguments $Seconds
  } finally {
    foreach ($entry in $saved.GetEnumerator()) {
      if ($null -eq $entry.Value) {
        Remove-Item -LiteralPath "Env:$($entry.Key)" -ErrorAction SilentlyContinue
      } else {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value)
      }
    }
  }
}

function Grant-FixturePermissions([string] $Exe, [string[]] $VmArguments,
    [string] $TestDex, [string] $Apk, [string] $Package, [string[]] $Permissions,
    [switch] $Installed, [int] $ExpectedUid = -1) {
  if ($Installed) {
    if ($ExpectedUid -lt 10000 -or $ExpectedUid -gt 19999) {
      throw "Installed fixture requires an ordinary application UID: $Package"
    }
    $identity = @('--remote', [string]$ExpectedUid, $Package)
  } else {
    $identity = @($Apk)
  }
  $arguments = $VmArguments + @('-cp', $TestDex,
    'gesher.services.FixturePermissionSetup') + $identity + $Permissions
  $out = Invoke-FixtureProcess $Exe $arguments 90 @{
    GESHER_UID = '1000'; GESHER_BINDER_UID = '1000'
  }
  $code = $LASTEXITCODE
  if ($Installed) {
    if ($code -ne 0 -or -not $out.Contains('FixturePermissionSetup live installed grants verified') -or
        -not $out.Contains("FixturePermissionSetup remote UID $ExpectedUid package $Package source ")) {
      throw "Installed fixture permission prerequisite failed: $out"
    }
    $uid = $ExpectedUid
  } else {
    $match = [regex]::Match($out, 'FixturePermissionSetup UID ([0-9]+) package ' + [regex]::Escape($Package) + '(\r?\n|$)')
    if ($code -ne 0 -or -not $out.Contains('FixturePermissionSetup persisted grants verified') -or -not $match.Success) {
      throw "Fixture permission prerequisite failed: $out"
    }
    $uid = [int]$match.Groups[1].Value
    if ($uid -lt 10000 -or $uid -gt 19999) { throw "Fixture did not receive an application UID: $Package" }
  }
  Write-Host $out
  return $uid
}
