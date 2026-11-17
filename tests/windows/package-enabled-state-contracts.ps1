# Actual ART, generated AIDL Parcel path, production enabled-state store and native
# shared cache region. Separate write/read VMs prove persistence across process exit.
$enabledSaved = @{}
foreach ($enabledName in @('ANDROID_DATA', 'GESHER_BINDER_UID', 'GESHER_UID', 'GESHER_MICROG_SPOOF')) {
  $enabledSaved[$enabledName] = [Environment]::GetEnvironmentVariable($enabledName, 'Process')
}
$enabledRoot = Join-Path $root ('package-enabled-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $enabledRoot -ErrorAction Stop | Out-Null
try {
  $env:GESHER_UID = '1000'
  $env:GESHER_BINDER_UID = '1000'
  foreach ($enabledMode in @('0', '1')) {
    $enabledData = Join-Path $enabledRoot ('data-' + $enabledMode)
    New-Item -ItemType Directory -Path $enabledData -ErrorAction Stop | Out-Null
    $enabledFull = [IO.Path]::GetFullPath($enabledData)
    if ($enabledFull -notmatch '^([A-Za-z]):[\\/]') { throw 'Enabled-state fixture needs a local drive path' }
    $env:ANDROID_DATA = '/mnt/' + $Matches[1].ToLowerInvariant() + ($enabledFull.Substring(2) -replace '\\', '/')
    $env:GESHER_MICROG_SPOOF = $enabledMode
    foreach ($enabledGroup in @('all', 'write', 'read')) {
      $enabledOutput = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
        'PackageEnabledStateContractTest' + $enabledGroup) 120
      $enabledCode = $LASTEXITCODE
      Check "packages: enabled-state $enabledGroup, mode=$enabledMode, actual ART and AIDL" {
        $global:LASTEXITCODE = $enabledCode
        $enabledOutput
      } @('Package enabled state contracts passed:', 'AIDL transactions=')
    }
    $enabledOutput = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
      'PackageEnabledLifecycleTest' + 'all') 120
    $enabledCode = $LASTEXITCODE
    Check "packages: enabled-state lifecycle, mode=$enabledMode, recording app threads" {
      $global:LASTEXITCODE = $enabledCode
      $enabledOutput
    } @('Package enabled lifecycle contracts passed:', 'actual ART and PM AIDL')
  }
} finally {
  foreach ($enabledName in $enabledSaved.Keys) {
    [Environment]::SetEnvironmentVariable($enabledName, $enabledSaved[$enabledName], 'Process')
  }
  Remove-Item -LiteralPath $enabledRoot -Recurse -Force -ErrorAction Stop
}
