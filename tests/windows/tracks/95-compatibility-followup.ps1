# Focused follow-up contracts, shared by the full and framework suites.
# RunTimed, FixesRun, Check and the actual ART boot arguments come from 50/94.

foreach ($fixture in @(
  @{ Main = 'NetworkStatsBucketContractTest'; Marker = 'NetworkStatsBucketContractTest all passed: 61';
     Label = 'SDK NetworkStats Bucket fields, getters and inherited calls' },
  @{ Main = 'NetworkRequestClearContractTest'; Marker = 'NetworkRequestClearContractTest all passed:';
     Label = 'NetworkRequest clear removes every retained filter' }
)) {
  $followupOut = FixesRun ($dvmFixes + ('gesher.services.' + $fixture.Main)) 90
  $followupCode = $LASTEXITCODE
  Check ('framework: ' + $fixture.Label) {
    $global:LASTEXITCODE = $followupCode
    $followupOut
  } @($fixture.Marker)
}

$followupSavedEnvironment = @{}
foreach ($name in 'ANDROID_DATA', 'GESHER_UID', 'GESHER_BINDER_UID', 'GESHER_MICROG_SPOOF',
                  'GESHER_IP_GEOLOCATION', 'TEMP', 'GESHER_METADATA_OTHER_VOLUME') {
  $followupSavedEnvironment[$name] = @{
    Present = Test-Path "Env:$name"
    Value = [Environment]::GetEnvironmentVariable($name, 'Process')
  }
}
$followupRoot = Join-Path $env:RUNNER_TEMP ('compatibility-followup-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $followupRoot | Out-Null
try {
  $env:GESHER_MICROG_SPOOF = '0'
  $env:GESHER_IP_GEOLOCATION = '0'
  $locationData = Join-Path $followupRoot 'location-data'
  New-Item -ItemType Directory -Path $locationData | Out-Null
  $env:ANDROID_DATA = NoDrive $locationData
  $locationOut = FixesRun ($dvmFixes + 'gesher.services.LocationRegistrationContractTest') 90
  $locationCode = $LASTEXITCODE
  Check 'services: provider-scoped location registration, Binder death and public client delivery' {
    $global:LASTEXITCODE = $locationCode
    $locationOut
  } @('LocationRegistrationContractTest all passed:')

  $shortcutData = Join-Path $followupRoot 'shortcut-data'
  New-Item -ItemType Directory -Path $shortcutData | Out-Null
  $env:ANDROID_DATA = NoDrive $shortcutData
  $env:GESHER_UID = '1000'
  $env:GESHER_BINDER_UID = '1000'
  $shortcutOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' + 'DynamicShortcutTest' +
    (NoDrive "$Dir/orderedbroadcast-source.apk") + (NoDrive "$Dir/orderedbroadcast-target.apk")) 120
  $shortcutCode = $LASTEXITCODE
  Check 'services: partial shortcut updates retain omitted fields through actual durable publication' {
    $global:LASTEXITCODE = $shortcutCode
    $shortcutOut
  } @('DynamicShortcutTest all passed:')

  # Both volumes and real symlinks are required: the C fixture fails if either
  # capability is missing. Each invocation creates and removes its own children.
  $env:TEMP = $followupRoot
  $env:GESHER_METADATA_OTHER_VOLUME = if ([IO.Path]::GetPathRoot($followupRoot) -ieq 'C:\') {
    'D:\'
  } else { Join-Path $env:SystemRoot 'Temp' }
  if (-not (Test-Path -LiteralPath $env:GESHER_METADATA_OTHER_VOLUME -PathType Container)) {
    throw 'The filesystem contract requires an available second volume'
  }
  foreach ($fixture in @(
    @{ Exe = 'filesystem_metadata_host_test.exe'; Args = @(); Label = 'host POSIX'; End = '0 failures' },
    @{ Exe = 'elf_load_test.exe'; Args = @("$Dir/filesystem_contract.so"); Label = 'x86_64 guest'; End = 'elf_load_test all passed' },
    @{ Exe = 'arm64_elf_test.exe'; Args = @("$Dir/filesystem_contract_arm64.so"); Label = 'ARM64 guest'; End = 'arm64_elf_test all passed' }
  )) {
    $metadataOut = RunTimed "$Dir/$($fixture.Exe)" $fixture.Args 90
    $metadataCode = $LASTEXITCODE
    Check ('filesystem: ' + $fixture.Label + ' timestamps, symlinks, Unicode, long paths and cross-volume rename') {
      $global:LASTEXITCODE = $metadataCode
      $metadataOut
    } @('filesystem metadata contracts:', ' checks, 0 failures',
        'filesystem metadata: cross-volume rename and renameat exercised', $fixture.End)
  }
} finally {
  foreach ($name in $followupSavedEnvironment.Keys) {
    $saved = $followupSavedEnvironment[$name]
    if ($saved.Present) { Set-Item "Env:$name" $saved.Value }
    else { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
  }
  Remove-Item -LiteralPath $followupRoot -Recurse -Force -ErrorAction Stop
}

foreach ($fixture in @(
  @{ Exe = 'media_source_contract_test.exe'; Marker = 'media source contracts:';
     Label = 'FD source range limits, positioned reads and duplicate ownership' },
  @{ Exe = 'media_surface_timestamp_contract_test.exe'; Marker = 'surface timestamp contracts:';
     Label = 'actual QueueSurface auto sentinel and persistent explicit timestamps' }
)) {
  $mediaOut = RunTimed "$Dir/$($fixture.Exe)" @() 90
  $mediaCode = $LASTEXITCODE
  Check ('media: ' + $fixture.Label) {
    $global:LASTEXITCODE = $mediaCode
    $mediaOut
  } @($fixture.Marker, ' checks, 0 failures')
}

function Invoke-FollowupHttpContracts {
  $owned = Join-Path $env:RUNNER_TEMP ('media-http-followup-' + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $owned | Out-Null
  $ready = Join-Path $owned 'port'
  $server = $null
  $code = -1
  try {
    $start = [Diagnostics.ProcessStartInfo]::new((Get-Command python -ErrorAction Stop).Source)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    foreach ($argument in @((Join-Path $PSScriptRoot '../../media/http_contract_server.py'), '--ready', $ready)) {
      $start.ArgumentList.Add($argument)
    }
    $server = [Diagnostics.Process]::Start($start)
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path -LiteralPath $ready) -and -not $server.HasExited -and $watch.Elapsed.TotalSeconds -lt 10) {
      Start-Sleep -Milliseconds 25
    }
    if ($server.HasExited -or -not (Test-Path -LiteralPath $ready)) {
      throw 'Owned loopback HTTP peer did not become ready within ten seconds'
    }
    $port = [int]([IO.File]::ReadAllText($ready))
    if ($port -lt 1 -or $port -gt 65535) { throw 'Invalid loopback peer port' }
    $output = RunTimed "$Dir/media_http_source_contract_test.exe" @("http://127.0.0.1:$port") 90
    $code = $LASTEXITCODE
    $output
  } finally {
    if ($null -ne $server) {
      if (-not $server.HasExited) { $server.Kill($true) }
      if (-not $server.WaitForExit(5000)) { throw 'Owned loopback peer did not stop' }
      $server.Dispose()
    }
    Remove-Item -LiteralPath $owned -Recurse -Force -ErrorAction Stop
    $global:LASTEXITCODE = $code
  }
}
Check 'media: real WinHTTP range responses, whole-file fallback, concurrent readers and cache eviction' {
  Invoke-FollowupHttpContracts
} @('Windows HTTP source contracts:', ' checks, 0 failures')
