# Runtime contracts added by the November 14 Android/Windows audit.
# Track 50 supplies bounded RunTimed; track 94 supplies the framework boot path.
. "$PSScriptRoot/../pthread-attribute-contracts.ps1"
. "$PSScriptRoot/../package-enabled-state-contracts.ps1"

# Exercise synthetic Hebrew/English permission dialogs on a never-shown desktop.
# This is the production dialog implementation, with denial/keyboard/scroll checks.
$permissionDialogOut = RunTimed "$Dir/permission_dialog_test.exe" @() 150
$permissionDialogCode = $LASTEXITCODE
Check 'services: native permission dialog explicit consent and Hebrew/English controls' {
  $global:LASTEXITCODE = $permissionDialogCode
  $permissionDialogOut
} @('permission_dialog_test:', ' checks, 0 failures')

foreach ($serviceFixture in @('RunningTaskSnapshotTest', 'TaskStackContractTest', 'TextServicesContractTest', 'MediaRouterContractTest', 'HandwritingUnavailableContractTest')) {
  $serviceContractOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' + $serviceFixture) 90
  $serviceContractCode = $LASTEXITCODE
  Check "services: $serviceFixture public manager and generated Binder contracts" {
    $global:LASTEXITCODE = $serviceContractCode
    $serviceContractOut
  } @("$serviceFixture all passed:")
}

. "$PSScriptRoot/../media-text-contracts.ps1"
Check 'services: remote media/text managers, ordinary identity and Binder death' {
  Test-MediaTextTransport $Dir $dvmFixes $root
} @('MediaRouterContractTest all passed:', 'MediaTextRemoteTest all passed:',
    'MediaTextRemoteTest death cleanup passed')

$aaudioSaved = [Environment]::GetEnvironmentVariable('GESHER_AUDIO', 'Process')
try {
  $env:GESHER_AUDIO = 'null'
  foreach ($aaudioGuest in @('aaudio_builder_x64', 'aaudio_builder_arm64')) {
    $aaudioOut = RunTimed "$Dir/gesher-exec.exe" @('--elf', (Resolve-Path "$Dir/$aaudioGuest").Path, $aaudioGuest) 90
    $aaudioCode = $LASTEXITCODE
    Check "media: $aaudioGuest actual negotiation, callbacks and clock errors" {
      $global:LASTEXITCODE = $aaudioCode
      $aaudioOut
    } @('AAudio builder guest contracts:', ' checks, 0 failures',
        'AAudio lifecycle guest contracts: all passed')
  }
} finally {
  if ($null -eq $aaudioSaved) { Remove-Item Env:GESHER_AUDIO -ErrorAction SilentlyContinue }
  else { [Environment]::SetEnvironmentVariable('GESHER_AUDIO', $aaudioSaved, 'Process') }
}

$effectContractOut = FixesRun ($dvmFixes + 'gesher.services.AudioEffectAvailabilityTest') 90
$effectContractCode = $LASTEXITCODE
Check 'media: absent effect engine has typed capability queries and constructor failure' {
  $global:LASTEXITCODE = $effectContractCode
  $effectContractOut
} @('AudioEffectAvailabilityTest all passed:')

$callbackMediaOut = FixesRun ($dvmFixes + 'gesher.services.MediaCallbackContractTest') 120
$callbackMediaCode = $LASTEXITCODE
Check 'media: real callback WAV sources, PCM bytes, metadata and receiver release' {
  $global:LASTEXITCODE = $callbackMediaCode
  $callbackMediaOut
} @('MediaCallbackContractTest all passed:')

# The November 16 contracts run on actual ART/JNI and generated Binder proxies.
foreach ($finishFixture in @(
  @{ Class = 'ActivityFinishContractTest'; Marker = 'Activity finish Binder contract all passed:' },
  @{ Class = 'NullBindingContractTest'; Marker = 'Null binding contracts passed:' },
  @{ Class = 'ServiceRebindContractTest'; Marker = 'Service rebinding contracts passed:' }
)) {
  $finishOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' + $finishFixture.Class) 90
  $finishCode = $LASTEXITCODE
  Check "services: $($finishFixture.Class) public callbacks and Binder lifecycle contracts" {
    $global:LASTEXITCODE = $finishCode
    $finishOut
  } @($finishFixture.Marker)
}

$mediaStoreAccessOut = FixesRun ($dvmFixes + 'gesher.services.MediaStoreAccessContractTest') 90
$mediaStoreAccessCode = $LASTEXITCODE
Check 'services: MediaStore SDK linkage, AppOps and provider access contracts' {
  $global:LASTEXITCODE = $mediaStoreAccessCode
  $mediaStoreAccessOut
} @('MediaStoreAccessContractTest all passed:')

$receiverAudioSaved = [Environment]::GetEnvironmentVariable('GESHER_AUDIO', 'Process')
try {
  $env:GESHER_AUDIO = 'null'
  $receiverMediaOut = FixesRun ($dvmFixes + 'gesher.services.MediaReceiverLifecycleTest') 90
  $receiverMediaCode = $LASTEXITCODE
  Check 'media: public SoundPool and MediaRecorder release ownership' {
    $global:LASTEXITCODE = $receiverMediaCode
    $receiverMediaOut
  } @('MediaReceiverLifecycleTest all passed:')
} finally {
  if ($null -eq $receiverAudioSaved) { Remove-Item Env:GESHER_AUDIO -ErrorAction SilentlyContinue }
  else { [Environment]::SetEnvironmentVariable('GESHER_AUDIO', $receiverAudioSaved, 'Process') }
}

foreach ($lookupCase in @(
  @{ Exe = 'elf_load_test.exe'; Library = 'lookup_root.so'; Label = 'x86_64'; End = 'elf_load_test all passed' },
  @{ Exe = 'arm64_elf_test.exe'; Library = 'lookup_root_arm64.so'; Label = 'translated ARM64'; End = 'arm64_elf_test all passed' }
)) {
  $lookupOut = RunTimed "$Dir/$($lookupCase.Exe)" @((NoDrive "$Dir/$($lookupCase.Library)"), (NoDrive "$Dir")) 90
  $lookupCode = $LASTEXITCODE
  Check "native: $($lookupCase.Label) transitive dlsym and breadth-first precedence" {
    $global:LASTEXITCODE = $lookupCode
    $lookupOut
  } @('elf_lookup_graph all passed', $lookupCase.End)
}

$adviceModePresent = Test-Path Env:GESHER_MMAP_NO_PLACEHOLDERS
$adviceModeValue = [Environment]::GetEnvironmentVariable('GESHER_MMAP_NO_PLACEHOLDERS', 'Process')
try {
  foreach ($adviceMode in @('placeholders', 'plain mappings')) {
    if ($adviceMode -eq 'placeholders') { Remove-Item Env:GESHER_MMAP_NO_PLACEHOLDERS -ErrorAction SilentlyContinue }
    else { $env:GESHER_MMAP_NO_PLACEHOLDERS = '1' }
    foreach ($adviceCase in @(
      @{ Exe = 'madvise_probe.exe'; Args = @(); Label = 'host POSIX'; End = @() },
      @{ Exe = 'elf_load_test.exe'; Args = @((NoDrive "$Dir/madvise_contract.so")); Label = 'x86_64 guest'; End = @('elf_load_test all passed') },
      @{ Exe = 'arm64_elf_test.exe'; Args = @((NoDrive "$Dir/madvise_contract_arm64.so")); Label = 'ARM64 guest'; End = @('arm64_elf_test all passed') }
    )) {
      $adviceOut = RunTimed "$Dir/$($adviceCase.Exe)" $adviceCase.Args 120
      $adviceCode = $LASTEXITCODE
      Check "native: madvise provenance and file preservation, $($adviceCase.Label), $adviceMode" {
        $global:LASTEXITCODE = $adviceCode
        $adviceOut
      } (@('madvise contracts (Windows policy):', ' checks, 0 failures') + $adviceCase.End)
    }
  }
} finally {
  if ($adviceModePresent) { Set-Item Env:GESHER_MMAP_NO_PLACEHOLDERS $adviceModeValue }
  else { Remove-Item Env:GESHER_MMAP_NO_PLACEHOLDERS -ErrorAction SilentlyContinue }
}

# Real Windows URLMON parser and production dispatch; recording COM cannot
# create a browser or touch a profile, including in the negative control.
$storageNativeOut = RunTimed "$Dir/webstorage_native_contracts.exe" @() 30
$storageNativeCode = $LASTEXITCODE
Check 'webview: native origin admission, scoped parameters and completion leases' {
  $global:LASTEXITCODE = $storageNativeCode
  $storageNativeOut
} @('WebStorage native contracts:', ' checks, 0 failures')

$storageNegativeOut = RunTimed "$Dir/webstorage_native_negative.exe" @() 30
$storageNegativeCode = $LASTEXITCODE
Check 'webview: recording control detects missing native origin admission' {
  if ($storageNegativeCode -eq 1 -and $storageNegativeOut.Contains('FAIL: direct Host rejects invalid origin') -and
      $storageNegativeOut.Contains('WebStorage native contracts:')) {
    $global:LASTEXITCODE = 0
    'WebStorage native negative control: missing admission rejected; no browser used'
  } else {
    $global:LASTEXITCODE = 1
    $storageNegativeOut
  }
} @('WebStorage native negative control: missing admission rejected; no browser used')

# Private profile, real WebView2, and intercepted local responses at two origins.
$storageContractProfile = Join-Path ([IO.Path]::GetTempPath()) ('gesher-storage-contract-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $storageContractProfile -ErrorAction Stop | Out-Null
try {
  $storageContractOut = FixesRun ($dvmFixes + 'gesher.services.WebStorageOriginContractTest' + $storageContractProfile) 180
  $storageContractCode = $LASTEXITCODE
  Check 'webview: origin deletion preserves other sites and other storage types' {
    $global:LASTEXITCODE = $storageContractCode
    $storageContractOut
  } @('WebStorageOriginContractTest all passed:', 'two origins, IndexedDB, localStorage, cookies and cache')
} finally {
  $storageCleanupDeadline = [DateTime]::UtcNow.AddSeconds(10)
  while (Test-Path -LiteralPath $storageContractProfile) {
    try { Remove-Item -LiteralPath $storageContractProfile -Recurse -Force -ErrorAction Stop }
    catch {
      if ([DateTime]::UtcNow -ge $storageCleanupDeadline) { throw }
      Start-Sleep -Milliseconds 200
    }
  }
}
