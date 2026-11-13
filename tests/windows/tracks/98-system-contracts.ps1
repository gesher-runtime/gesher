# Android system contracts, in both the full and framework-only Windows suites.
# Track94 supplies FixesRun and the real ART boot arguments in $dvmFixes.

foreach ($fixture in @(
  @{ Exe = 'futex_test.exe'; Args = @(); End = 'futex_contract: all passed'; Label = 'host POSIX' },
  @{ Exe = 'futex_owner_link_test.exe'; Args = @(); End = 'futex_owner_link_test all passed'; Label = 'DLL-only executable owner' },
  @{ Exe = 'futex_bridge_test.exe'; Args = @(); End = 'futex_bridge_test all passed'; Label = 'mixed host, guest and PE DLL queues' },
  @{ Exe = 'elf_load_test.exe'; Args = @("$Dir/futex_contract.so"); End = 'elf_load_test all passed'; Label = 'x86_64 Android guest' },
  @{ Exe = 'arm64_elf_test.exe'; Args = @("$Dir/futex_contract_arm64.so"); End = 'arm64_elf_test all passed'; Label = 'ARM64 Android guest' }
)) {
  $futexOut = RunTimed "$Dir/$($fixture.Exe)" $fixture.Args 120
  $futexCode = $LASTEXITCODE
  $markers = @($fixture.End)
  if ($fixture.Args.Count) { $markers += 'futex_contract: all passed' }
  Check ('native: futex ' + $fixture.Label) {
    $global:LASTEXITCODE = $futexCode
    $futexOut
  } $markers
}

# The fixture's primary tree is relative to the inherited working directory.
# Require a real second volume; dropping the drive letter would hide FTW_MOUNT.
$traversalSavedPresent = Test-Path Env:GESHER_TRAVERSAL_SECOND_ROOT
$traversalSavedValue = [Environment]::GetEnvironmentVariable('GESHER_TRAVERSAL_SECOND_ROOT', 'Process')
$traversalWorkingVolume = [IO.Path]::GetPathRoot((Get-Location).Path)
$traversalOtherVolume = [IO.DriveInfo]::GetDrives() | Where-Object {
  $_.IsReady -and $_.DriveType -eq [IO.DriveType]::Fixed -and $_.RootDirectory.FullName -ine $traversalWorkingVolume
} | Select-Object -First 1
if (-not $traversalOtherVolume) { throw 'The traversal contract requires a writable second volume' }
$traversalSecondRoot = Join-Path $traversalOtherVolume.RootDirectory.FullName ('gesher-traversal-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $traversalSecondRoot -ErrorAction Stop | Out-Null
try {
  $env:GESHER_TRAVERSAL_SECOND_ROOT = $traversalSecondRoot -replace '\\', '/'
  foreach ($fixture in @(
    @{ Exe = 'traversal_test.exe'; Args = @(); End = 'traversal_contract: all passed'; Label = 'host POSIX' },
    @{ Exe = 'elf_load_test.exe'; Args = @("$Dir/traversal_contract.so"); End = 'elf_load_test all passed'; Label = 'x86_64 Android guest' },
    @{ Exe = 'arm64_elf_test.exe'; Args = @("$Dir/traversal_contract_arm64.so"); End = 'arm64_elf_test all passed'; Label = 'ARM64 Android guest' }
  )) {
    $traversalOut = RunTimed "$Dir/$($fixture.Exe)" $fixture.Args 120
    $traversalCode = $LASTEXITCODE
    Check ('native: directory traversal flags, callbacks and mount boundaries, ' + $fixture.Label) {
      $global:LASTEXITCODE = $traversalCode
      $traversalOut
    } @('traversal_contract: all passed', $fixture.End)
  }
} finally {
  if ($traversalSavedPresent) { Set-Item Env:GESHER_TRAVERSAL_SECOND_ROOT $traversalSavedValue }
  else { Remove-Item Env:GESHER_TRAVERSAL_SECOND_ROOT -ErrorAction SilentlyContinue }
  Remove-Item -LiteralPath $traversalSecondRoot -Recurse -Force -ErrorAction Stop
}

$systemContractEnvironment = @{}
foreach ($name in 'ANDROID_DATA', 'GESHER_UID', 'GESHER_BINDER_UID', 'GESHER_MICROG_SPOOF') {
  $systemContractEnvironment[$name] = @{
    Present = Test-Path "Env:$name"
    Value = [Environment]::GetEnvironmentVariable($name, 'Process')
  }
}
$systemContractRoot = Join-Path ([IO.Path]::GetTempPath()) ('gesher-system-contract-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $systemContractRoot -ErrorAction Stop | Out-Null
try {
  $env:ANDROID_DATA = NoDrive $systemContractRoot
  $env:GESHER_UID = '1000'
  $env:GESHER_BINDER_UID = '1000'
  $env:GESHER_MICROG_SPOOF = '0'
  $settingsBootstrapBinder = [Environment]::GetEnvironmentVariable('GESHER_BINDER', 'Process')
  try {
    Remove-Item -LiteralPath Env:GESHER_BINDER -ErrorAction SilentlyContinue
    $settingsBootstrapOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' + 'SettingsBootstrapContractTest') 90
    $settingsBootstrapCode = $LASTEXITCODE
    Check 'services: Settings context bootstrap without Binder and deferred worker transport' {
      $global:LASTEXITCODE = $settingsBootstrapCode
      $settingsBootstrapOut
    } @('SettingsBootstrapContractTest all passed:', 'bootstrap without Binder, worker transport')
  } finally {
    if ($null -eq $settingsBootstrapBinder) { Remove-Item -LiteralPath Env:GESHER_BINDER -ErrorAction SilentlyContinue }
    else { [Environment]::SetEnvironmentVariable('GESHER_BINDER', $settingsBootstrapBinder, 'Process') }
  }
  $settingsContractOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' + 'SettingsProviderContractTest') 90
  $settingsContractCode = $LASTEXITCODE
  Check 'services: durable Settings CRUD, nullable rows, errors and observer contracts' {
    $global:LASTEXITCODE = $settingsContractCode
    $settingsContractOut
  } @('Settings provider contracts passed:')

  # Use devsvc's private URI-grant provider without optional GmsCore metadata.
  $providerContractOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
    'ProviderSecurityTest' + (NoDrive (Join-Path $PSScriptRoot '../../dex-services/fixtures')) +
    (NoDrive "$Dir/devsvc.apk")) 120
  $providerContractCode = $LASTEXITCODE
  Check 'services: provider reference ownership, lock order and atomic retirement' {
    $global:LASTEXITCODE = $providerContractCode
    $providerContractOut
  } @('ProviderSecurityTest: provider counts, connection ownership, lock order and atomic retirement passed',
    'ProviderSecurityTest all passed')

  # Constructed package metadata uses the existing synthetic-fixture capability.
  $env:GESHER_MICROG_SPOOF = '1'
  $syncContractOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' + 'SyncManagerContractTest') 120
  $syncContractCode = $LASTEXITCODE
  Check 'services: sync admission, retry clocks, adapter workers and binding cleanup' {
    $global:LASTEXITCODE = $syncContractCode
    $syncContractOut
  } @('Sync manager contracts passed:')
  $env:GESHER_MICROG_SPOOF = '0'

  . "$PSScriptRoot/../settings-provider-contracts.ps1"
  Check 'services: ordinary-UID Settings Binder transport and reentrant observers' {
    Test-SettingsProviderTransport $Dir $dvmFixes $systemContractRoot
  } @('SettingsProviderRemoteTest all passed:', 'ordinary UID, BinderProxy')
} finally {
  foreach ($name in $systemContractEnvironment.Keys) {
    $saved = $systemContractEnvironment[$name]
    if ($saved.Present) { Set-Item "Env:$name" $saved.Value }
    else { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
  }
  Remove-Item -LiteralPath $systemContractRoot -Recurse -Force -ErrorAction Stop
}

# Cookie callbacks use a private profile and real WebView2; no network navigation.
. "$PSScriptRoot/../webview/Runtime.ps1"
$cookieContractProfile = Join-Path ([IO.Path]::GetTempPath()) ('gesher-cookie-contract-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $cookieContractProfile -ErrorAction Stop | Out-Null
try {
  $cookieContractOut = FixesRun ($dvmFixes + 'gesher.services.CookieCallbackContractTest' + $cookieContractProfile) 120
  $cookieContractCode = $LASTEXITCODE
  Check 'webview: cookie callbacks return to each calling HandlerThread' {
    $global:LASTEXITCODE = $cookieContractCode
    $cookieContractOut
  } @('CookieCallbackContractTest all passed:', 'real HandlerThreads and native WebView2 callbacks')
} finally {
  # WebView2 can retire its child process shortly after the VM exits. Remove only
  # this fixture's profile, with a finite retry; never kill unrelated browsers.
  $cookieCleanupDeadline = [DateTime]::UtcNow.AddSeconds(10)
  while (Test-Path -LiteralPath $cookieContractProfile) {
    try { Remove-Item -LiteralPath $cookieContractProfile -Recurse -Force -ErrorAction Stop }
    catch {
      if ([DateTime]::UtcNow -ge $cookieCleanupDeadline) { throw }
      Start-Sleep -Milliseconds 200
    }
  }
}
