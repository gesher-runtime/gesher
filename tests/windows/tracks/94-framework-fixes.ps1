# Regression checks of app-visible fixes in gesher-app's services and boot class
# path, run from services-tests.dex (tests/dex-services) with the interpreter.
# Dot-sourced by run-tests.ps1, using Check, NoDrive, $Dir, $root and $failed
# from there.

# Use track50's shared bounded process and redirected-reader cleanup. Waiting
# on Task.Result after an expired reader wait can otherwise hang this track
# when a descendant retains a pipe, even though the VM itself has exited.
function FixesRun([string[]] $Arguments, [int] $Seconds) {
  RunTimed "$Dir/dalvikvm.exe" $Arguments $Seconds
}

Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework') -Force
Copy-Item "$Dir/build.prop" (Join-Path $root 'system') -Force
$bcpFixes = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmFixes = @("-Xbootclasspath:$bcpFixes", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false',
  '-cp', (NoDrive "$Dir/services-tests.dex"))

$httpEngineOut = FixesRun ($dvmFixes + 'HttpEngineDeviceSmoke') 45
$httpEngineCode = $LASTEXITCODE
Check 'network: Android HttpEngine requests over real ART and HTTP sockets' {
  $global:LASTEXITCODE = $httpEngineCode
  $httpEngineOut
} @('HttpEngineDeviceSmoke passed:')

foreach ($httpCacheTest in @('HttpEngineCacheTest', 'HttpEngineConnectionLifetimeTest', 'HttpEngineReadBufferTest')) {
  $httpCacheOut = FixesRun ($dvmFixes + $httpCacheTest) 90
  $httpCacheCode = $LASTEXITCODE
  Check "network: $httpCacheTest real-provider contracts on ART" {
    if ($httpCacheCode -ne 0) { throw "HTTP engine contract failed: $httpCacheOut" }
    $httpCacheOut
  } @("${httpCacheTest} passed:")
}

foreach ($httpPolicyMode in @('ordinary', 'global-enabled', 'global-disabled')) {
  $httpPolicyOut = FixesRun ($dvmFixes + 'HttpEngineProtocolPolicyTest' + $httpPolicyMode) 45
  $httpPolicyCode = $LASTEXITCODE
  Check "network: HTTP/2 $httpPolicyMode engine isolation and request/response headers" {
    if ($httpPolicyCode -ne 0) { throw "HTTP protocol policy failed: $httpPolicyOut" }
    $httpPolicyOut
  } @('HttpEngineProtocolPolicyTest passed:')
}

$http2Out = FixesRun ($dvmFixes + 'Http2DeviceSmoke') 45
$http2Code = $LASTEXITCODE
Check 'network: HTTP/2 full duplex, direct buffers, flush and gRPC trailers' {
  $global:LASTEXITCODE = $http2Code
  $http2Out
} @('Http2DeviceSmoke passed:')

$http2ContractModes = @('gzip-headers', 'early-read', 'readonly-write', 'large', 'cancel', 'disabled', 'sessions')
foreach ($http2ContractMode in $http2ContractModes) {
  $http2ContractOut = FixesRun ($dvmFixes + 'Http2DeviceSmoke' + $http2ContractMode) 45
  $http2ContractCode = $LASTEXITCODE
  Check "network: HTTP/2 $http2ContractMode callback and buffer contract" {
    $global:LASTEXITCODE = $http2ContractCode
    $http2ContractOut
  } @('Http2DeviceSmoke passed:')
}

$http2CreditOut = FixesRun ($dvmFixes + 'Http2CreditContracts') 45
$http2CreditCode = $LASTEXITCODE
Check 'network: HTTP/2 returns discarded receive credit exactly once' {
  if ($http2CreditCode -ne 0) { throw "HTTP/2 receive credit failed: $http2CreditOut" }
  $http2CreditOut
} @('Http2CreditContracts: 55 checks, 0 failures; in-memory only, max DATA 1025 bytes')

$http2GzipOut = FixesRun ($dvmFixes + 'Http2DeviceSmoke' + 'gzip') 45
$http2GzipCode = $LASTEXITCODE
Check 'network: HTTP/2 content decoding before gRPC message delivery' {
  $global:LASTEXITCODE = $http2GzipCode
  $http2GzipOut
} @('Http2DeviceSmoke passed:')

$http2UnaryOut = FixesRun ($dvmFixes + 'Http2DeviceSmoke' + 'unary') 45
$http2UnaryCode = $LASTEXITCODE
Check 'network: gRPC trailers complete without an extra application read' {
  $global:LASTEXITCODE = $http2UnaryCode
  $http2UnaryOut
} @('Http2DeviceSmoke passed:')

$launcherLogBoundsOut = FixesRun ($dvmFixes + 'gesher.services.LauncherLogBoundsTest') 90
$launcherLogBoundsCode = $LASTEXITCODE
Check 'launcher: Android file channels, UTF-8 tails and bounded diagnostic lines' {
  $global:LASTEXITCODE = $launcherLogBoundsCode
  $launcherLogBoundsOut
} @('LauncherLogBoundsTest all passed:')

# Constructed package/permission state uses the same explicit test capability as
# the existing synthetic service fixtures. Keep it local to this fresh VM.
$pendingSpoofPresent = Test-Path Env:GESHER_MICROG_SPOOF
$pendingSpoofValue = [Environment]::GetEnvironmentVariable('GESHER_MICROG_SPOOF', 'Process')
try {
  $env:GESHER_MICROG_SPOOF = '1'
  $pendingCallbacksOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
    'PendingIntentContractTest') 90
  $pendingCallbacksCode = $LASTEXITCODE
} finally {
  if ($pendingSpoofPresent) { Set-Item Env:GESHER_MICROG_SPOOF $pendingSpoofValue }
  else { Remove-Item Env:GESHER_MICROG_SPOOF -ErrorAction SilentlyContinue }
}
Check 'services: PendingIntent identity, fill-in, updates and completion/cancellation callbacks' {
  $global:LASTEXITCODE = $pendingCallbacksCode
  $pendingCallbacksOut
} @('PendingIntentContractTest all passed:')

$clipboardCallbacksOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
  'ClipboardCallbackTest') 90
$clipboardCallbacksCode = $LASTEXITCODE
Check 'services: concurrent clipboard callbacks and native Windows clipboard updates' {
  $global:LASTEXITCODE = $clipboardCallbacksCode
  $clipboardCallbacksOut
} @('ClipboardCallbackTest all passed:')

$clipboardSecurityOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
  'ClipboardSecurityContractTest') 90
$clipboardSecurityCode = $LASTEXITCODE
Check 'services: clipboard security caller focus check and default IME contract' {
  $global:LASTEXITCODE = $clipboardSecurityCode
  $clipboardSecurityOut
} @('ClipboardSecurityContractTest all passed:')

$receiverExportOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
  'ReceiverExportContractTest') 90
$receiverExportCode = $LASTEXITCODE
Check 'services: dynamic receiver exports, protected broadcasts and sticky-query exemptions' {
  $global:LASTEXITCODE = $receiverExportCode
  $receiverExportOut
} @('ReceiverExportContractTest passed:', 'binder=true')

$contentObserverOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
  'ContentObserverLifecycleTest') 90
$contentObserverCode = $LASTEXITCODE
Check 'services: content observer publication, retirement and callback isolation' {
  $global:LASTEXITCODE = $contentObserverCode
  $contentObserverOut
} @('ContentObserverLifecycleTest all passed:')

$schedulerWorkOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
  'SchedulerWorkContractTest') 90
$schedulerWorkCode = $LASTEXITCODE
Check 'services: queued job completion, stopping work and alarm-clock replacement' {
  $global:LASTEXITCODE = $schedulerWorkCode
  $schedulerWorkOut
} @('SchedulerWorkContractTest all passed:')

$schedulerConstraintsOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
  'JobSchedulerConstraintContractTest') 90
$schedulerConstraintsCode = $LASTEXITCODE
Check 'services: observed job constraints, deadlines, callback ordering and bounded retirement' {
  $global:LASTEXITCODE = $schedulerConstraintsCode
  $schedulerConstraintsOut
} @('JobSchedulerConstraintContractTest all passed:')

$activityRestoreOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
  'ActivityRestoreContractTest') 90
$activityRestoreCode = $LASTEXITCODE
Check 'services: activity state restoration, process death survival and LaunchActivityItem delivery' {
  $global:LASTEXITCODE = $activityRestoreCode
  $activityRestoreOut
} @('ActivityRestoreContractTest all passed:')

$mediaRequestsOut = FixesRun ($dvmFixes + 'gesher.services.MediaStoreRequestContractTest') 90
$mediaRequestsCode = $LASTEXITCODE
Check 'framework: MediaStore request forwarding, provider errors and SDK callback descriptors' {
  $global:LASTEXITCODE = $mediaRequestsCode
  $mediaRequestsOut
} @('MediaStoreRequestContractTest all passed:')

# Public media/graphics APIs exercise the actual Windows extractor, JPEG codec,
# mapped storage and JNI registration, beyond the production-source host probes.
$mediaDataOut = FixesRun ($dvmFixes + 'gesher.services.MediaDataContractTest') 90
$mediaDataCode = $LASTEXITCODE
Check 'media: extractor source ownership and cropped YUV JPEG pixels' {
  $global:LASTEXITCODE = $mediaDataCode
  $mediaDataOut
} @('MediaDataContractTest all passed:')

$mediaSourceCallbacksOut = FixesRun ($dvmFixes + 'gesher.services.MediaDataSourceContractTest') 90
$mediaSourceCallbacksCode = $LASTEXITCODE
Check 'media: callback data sources through extractor, retriever and player JNI' {
  $global:LASTEXITCODE = $mediaSourceCallbacksCode
  $mediaSourceCallbacksOut
} @('MediaDataSourceContractTest all passed:')

$nsdValuesOut = FixesRun ($dvmFixes + 'gesher.services.NsdValueContractTest') 90
$nsdValuesCode = $LASTEXITCODE
Check 'network: NSD TXT limits and actual Parcel address/value round trips' {
  $global:LASTEXITCODE = $nsdValuesCode
  $nsdValuesOut
} @('NsdValueContractTest all passed:', 'parcel=true')

$bitmapCapacityOut = FixesRun ($dvmFixes + 'gesher.services.BitmapParcelCapacityTest') 90
$bitmapCapacityCode = $LASTEXITCODE
Check 'graphics: Bitmap Parcel pixels and actual SharedMemory capacity' {
  $global:LASTEXITCODE = $bitmapCapacityCode
  $bitmapCapacityOut
} @('BitmapParcelCapacityTest all passed:')

$nativePathsOut = FixesRun ($dvmFixes + 'gesher.services.NativeLibraryPathsTest') 90
$nativePathsCode = $LASTEXITCODE
Check 'installer: native library filenames and failed extraction preservation' {
  $global:LASTEXITCODE = $nativePathsCode
  $nativePathsOut
} @('NativeLibraryPathsTest all passed:')

$standInDispatchOut = FixesRun ($dvmFixes + 'gesher.services.StandInDispatchTest') 90
$standInDispatchCode = $LASTEXITCODE
Check 'services: cached dispatch preserves overloads, defaults, strict mode and concurrent calls' {
  $global:LASTEXITCODE = $standInDispatchCode
  $standInDispatchOut
} @('StandInDispatchTest all passed:')

$webviewPathsOut = FixesRun ($dvmFixes + 'gesher.services.WebViewUploadPathsTest') 90
$webviewPathsCode = $LASTEXITCODE
Check 'webview: upload paths share the drive and UNC namespace' {
  $global:LASTEXITCODE = $webviewPathsCode
  $webviewPathsOut
} @('WebViewUploadPathsTest all passed:')

# Real temporary storage in fresh VMs, including explicit overrides and app cache.
. "$PSScriptRoot/../temp-directory-contracts.ps1"
Check 'runtime: real temporary directories, explicit overrides, app cache and I/O errors' {
  Test-TemporaryDirectoryContracts $Dir $dvmFixes
} @('TempDirectoryBootstrapTest all passed: 10 fresh VM scenarios')

# The canonical UNC namespace must reach actual files, including the VM's temp default.
. "$PSScriptRoot/../unc-file-contracts.ps1"
Check 'runtime: UNC File/NIO operations and temporary storage on a private localhost share' {
  Test-UncFileContracts $Dir $dvmFixes $root
} @('UncFileBootstrapTest all passed: real file and temporary-directory I/O; share removed')

# Query through the real PackageManager client and generated Binder parcels, using
# properties parsed from this build's signed synthetic components.apk, plus the
# existing unsigned APK for the unverified system-addon rejection contract.
$packageQueryOut = FixesRun ($dvmFixes + 'gesher.services.PackageQueryContractTest' +
  (NoDrive "$Dir/components.apk"),
  (NoDrive (Join-Path $PSScriptRoot '../../dex-services/fixtures/unsigned.apk'))) 90
$packageQueryCode = $LASTEXITCODE
Check 'services: package versions, permissions, identities and parsed manifest properties' {
  $global:LASTEXITCODE = $packageQueryCode
  $packageQueryOut
} @('PackageQueryContractTest all passed:')

# Distinct package identities and real manifest visibility rules, through the
# generated Binder and public manager. Keep the fixture's trusted setup identity
# local to its fresh VM and restore absent versus empty parent values exactly.
$visibilitySavedEnvironment = @{}
foreach ($visibilityName in 'GESHER_UID', 'GESHER_BINDER_UID') {
  $visibilitySavedEnvironment[$visibilityName] = @{
    Present = Test-Path "Env:$visibilityName"
    Value = [Environment]::GetEnvironmentVariable($visibilityName, 'Process')
  }
}
try {
  $env:GESHER_UID = '1000'
  $env:GESHER_BINDER_UID = '1000'
  $packageVisibilityOut = FixesRun ($dvmFixes + 'gesher.services.PackageVisibilityContractTest') 90
  $packageVisibilityCode = $LASTEXITCODE
} finally {
  foreach ($visibilityName in 'GESHER_UID', 'GESHER_BINDER_UID') {
    $visibilitySaved = $visibilitySavedEnvironment[$visibilityName]
    if ($visibilitySaved.Present) { Set-Item "Env:$visibilityName" $visibilitySaved.Value }
    else { Remove-Item "Env:$visibilityName" -ErrorAction SilentlyContinue }
  }
}
Check 'services: actual package visibility, manifest queries and public query exceptions' {
  $global:LASTEXITCODE = $packageVisibilityCode
  $packageVisibilityOut
} @('PackageVisibilityContractTest all passed:')

$activityOptionsOut = FixesRun ($dvmFixes + 'gesher.services.ActivityOptionsContractTest' +
  (NoDrive "$Dir/components.apk")) 90
$activityOptionsCode = $LASTEXITCODE
Check 'services: real activity options, specific intent ordering and manifest filtering' {
  $global:LASTEXITCODE = $activityOptionsCode
  $activityOptionsOut
} @('ActivityOptionsContractTest all passed:')

# A real installed base plus two isolated feature APKs, including an inter-feature
# dependency. Reject malformed updates before publishing or stopping the old app.
$splitPackageOut = FixesRun ($dvmFixes + 'gesher.services.SplitPackageContractTest' +
  (NoDrive "$Dir/split-package-fixtures.zip")) 150
$splitPackageCode = $LASTEXITCODE
Check 'services: installed feature manifests, isolated split DEX/resources and failed-update preservation' {
  $global:LASTEXITCODE = $splitPackageCode
  $splitPackageOut
} @('SplitPackageContractTest all passed:')

# Parser loaders own the APK mappings they create. Keep strong references alive
# across close, then require real source-file truncation without collection.
$parserAssetsOut = FixesRun ($dvmFixes + 'gesher.services.ParserAssetLifecycleTest' +
  (NoDrive "$Dir/split-package-fixtures.zip")) 90
$parserAssetsCode = $LASTEXITCODE
Check 'framework: parser APK ownership, partial failure cleanup and immediate source rewrite' {
  $global:LASTEXITCODE = $parserAssetsCode
  $parserAssetsOut
} @('ParserAssetLifecycleTest all passed:')

# Channel/group policy is exercised through the real manager and Binder parcels,
# including durable reload and publication/lifecycle races. The fixture owns a
# disposable child data directory and needs the trusted service UID for its
# retained-toast checks.
$groupsSavedBinderUid = $env:GESHER_BINDER_UID
try {
  $env:GESHER_BINDER_UID = '1000'
  $groupsOut = FixesRun ($dvmFixes + 'gesher.services.NotificationChannelGroupsTest' +
    (NoDrive "$Dir/devsvc.apk")) 120
  $groupsCode = $LASTEXITCODE
} finally {
  if ($null -eq $groupsSavedBinderUid) { Remove-Item Env:GESHER_BINDER_UID -ErrorAction SilentlyContinue }
  else { $env:GESHER_BINDER_UID = $groupsSavedBinderUid }
}
Check 'services: durable notification groups, real channel policy and lifecycle deletion guards' {
  $global:LASTEXITCODE = $groupsCode
  $groupsOut
} @('NotificationChannelGroupsTest all passed:')

# Delegation uses the real service and generated Binder parcels with controlled
# fixture identities, plus durable restart/migration and ownership checks.
$delegationOut = FixesRun ($dvmFixes + 'gesher.services.NotificationDelegationTest') 120
$delegationCode = $LASTEXITCODE
Check 'services: authenticated notification delegation, revocation and retained poster ownership' {
  $global:LASTEXITCODE = $delegationCode
  $delegationOut
} @('NotificationDelegationTest all passed:')

# The format fixture calls package-private production storage methods. Put this
# test DEX on this VM's boot path, using a fresh profile and no application UI.
$formatData = Join-Path $root 'notification-format-data'
New-Item -ItemType Directory -Path $formatData -Force | Out-Null
$formatSavedData = $env:ANDROID_DATA
$formatSavedSpoof = $env:GESHER_MICROG_SPOOF
try {
  $env:ANDROID_DATA = NoDrive $formatData
  $env:GESHER_MICROG_SPOOF = '0'
  $formatBoot = $bcpFixes + ':' + (NoDrive "$Dir/services-tests.dex")
  $formatArgs = @("-Xbootclasspath:$formatBoot", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false',
    'com.gesher.server.NotificationStoreFormatRunner')
  foreach ($formatPhase in @('write', 'read')) {
    $formatOut = FixesRun ($formatArgs + $formatPhase) 90
    $formatCode = $LASTEXITCODE
    Check "services: native Parcel notification formats and restart migration ($formatPhase)" {
      $global:LASTEXITCODE = $formatCode
      $formatOut
    } @("NotificationStoreFormatDevice $formatPhase passed:")
  }
} finally {
  if ($null -eq $formatSavedData) { Remove-Item Env:ANDROID_DATA -ErrorAction SilentlyContinue }
  else { $env:ANDROID_DATA = $formatSavedData }
  if ($null -eq $formatSavedSpoof) { Remove-Item Env:GESHER_MICROG_SPOOF -ErrorAction SilentlyContinue }
  else { $env:GESHER_MICROG_SPOOF = $formatSavedSpoof }
}

# Real JobService callbacks attach notifications to a running job. Exercise
# shared ownership, replacement, completion and cancellation through generated
# Binder transactions, including their retained-notification effects.
$jobsSavedBinderUid = $env:GESHER_BINDER_UID
try {
  $env:GESHER_BINDER_UID = '1000'
  $jobNotificationsOut = FixesRun ($dvmFixes + 'gesher.services.JobNotificationLifecycleTest' +
    (NoDrive "$Dir/devsvc.apk")) 120
  $jobNotificationsCode = $LASTEXITCODE
} finally {
  if ($null -eq $jobsSavedBinderUid) { Remove-Item Env:GESHER_BINDER_UID -ErrorAction SilentlyContinue }
  else { $env:GESHER_BINDER_UID = $jobsSavedBinderUid }
}
Check 'services: job notification validation, shared ownership and real completion cleanup' {
  $global:LASTEXITCODE = $jobNotificationsCode
  $jobNotificationsOut
} @('JobNotificationLifecycleTest all passed:')

# Validate focus ownership and callbacks through the production service, generated
# Binder transactions, and the real AudioManager listener/Handler path.
$audioFocusOut = FixesRun ($dvmFixes + 'gesher.services.AudioFocusLifecycleTest') 90
$audioFocusCode = $LASTEXITCODE
Check 'services: audio focus ownership, loss escalation, token death and public manager callbacks' {
  $global:LASTEXITCODE = $audioFocusCode
  $audioFocusOut
} @('AudioFocusLifecycleTest all passed:')

# UiMode uses generated IUiModeManager Binder parcels and the real ATM transaction path.
$uiModeOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
  'UiModeContractTest') 90
$uiModeCode = $LASTEXITCODE
Check 'services: Android night mode scope, schedule, permissions, persistence and relaunch' {
  $global:LASTEXITCODE = $uiModeCode
  $uiModeOut
} @('UiModeContractTest all passed:', 'checks; binder=true')

# The wrapper creates the writable framework cache region. The fixture uses
# generated AM Binder parcels and production URI/provider authorization state.
$uriGrantsOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
  'UriGrantContractTest') 90
$uriGrantsCode = $LASTEXITCODE
Check 'services: URI delegation, provider patterns, exact identities and scoped revocation' {
  $global:LASTEXITCODE = $uriGrantsCode
  $uriGrantsOut
} @('UriGrantContractTest all passed: 154 checks; binder=true')

# Direct-service fixture: actual metadata publication, persisted record reload,
# owner retirement, shared UID, file IO concurrency, admin types and quota.
# Actual APK removal/reinstall is covered by PackageUninstallTest separately;
# this fixture does not claim to exercise Binder transport or process teardown.
$persistableUriGrantsOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
  'PersistableUriGrantContractTest') 90
$persistableUriGrantsCode = $LASTEXITCODE
Check 'services: persisted URI records, release, reload and quota' {
  $global:LASTEXITCODE = $persistableUriGrantsCode
  $persistableUriGrantsOut
} @('PersistableUriGrantContractTest all passed: 99 checks')

$appOpsOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
  'AppOpsContractTest') 90
$appOpsCode = $LASTEXITCODE
Check 'services: AppOps defaults, authority, attributed delivery, foreground lifecycle and signed persistence' {
  $global:LASTEXITCODE = $appOpsCode
  $appOpsOut
} @('AppOpsContractTest all passed: 489 checks; binder=true')

# Production foreground-service permission/type admission through generated AM parcels.
# Parsed APK metadata and the notification display sink are isolated fixture boundaries.
$foregroundAdmissionOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
  'ForegroundServiceAdmissionTest') 90
$foregroundAdmissionCode = $LASTEXITCODE
Check 'services: foreground-service manifest permission and type admission' {
  $global:LASTEXITCODE = $foregroundAdmissionCode
  $foregroundAdmissionOut
} @('ForegroundServiceAdmissionTest all passed:', 'checks; binder=true')

$uidIsolationOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
  'PackageUidIsolationContractTest' +
  (NoDrive (Join-Path $PSScriptRoot '../../dex-services/fixtures/v2only.apk'))) 90
$uidIsolationCode = $LASTEXITCODE
Check 'services: per-package identity, distinct UID allocation, sharedUserId and isolation' {
  $global:LASTEXITCODE = $uidIsolationCode
  $uidIsolationOut
} @('PackageUidIsolationContractTest passed:')

$intentUriGrantsOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
  'IntentUriGrantContractTest') 90
$intentUriGrantsCode = $LASTEXITCODE
Check 'services: Intent URI grants, IBinder owner lifecycle and non-system ignore' {
  $global:LASTEXITCODE = $intentUriGrantsCode
  $intentUriGrantsOut
} @('IntentUriGrantContractTest passed: 28 checks')

# Caller identity and URI snapshots must survive actual ActivityThread delivery,
# relaunch and results, including a PendingIntent holder cleared by its own send.
. "$PSScriptRoot/../component-caller-contracts.ps1"
Check 'services: ComponentCaller identity, URI access, callbacks, relaunch and delegated delivery' {
  Test-ComponentCallerContracts $Dir $dvmFixes $root
} @('ComponentCallerBootstrapTest all passed: real Binder and cross-app callbacks; owned device stopped')

# Exercise the MediaStore implementation from the production boot jar.
$mediaStoreOut = FixesRun ($dvmFixes + 'gesher.services.MediaStoreContractTest') 90
$mediaStoreCode = $LASTEXITCODE
Check 'services: MediaStore picker/collection contracts and missing-provider handling' {
  $global:LASTEXITCODE = $mediaStoreCode
  $mediaStoreOut
} @('MediaStoreContractTest all passed: 28')

# Production DocumentsProvider transport and real filesystem operations. The
# explicit package fixture capability is confined to these fresh ART processes.
. "$PSScriptRoot/../saf-binder-transport.ps1"
$safSpoofPresent = Test-Path Env:GESHER_MICROG_SPOOF
$safSpoofValue = [Environment]::GetEnvironmentVariable('GESHER_MICROG_SPOOF', 'Process')
$safRestart = Join-Path $root ('saf-restart-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $safRestart -Force | Out-Null
$safBinderSession = $null
try {
  $env:GESHER_MICROG_SPOOF = '1'
  $safBinderSession = Start-SafBinderFixture $Dir $dvmFixes $root
  $safOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
    'SafDocumentsContractTest') 90
  $safCode = $LASTEXITCODE
  Check 'services: AOSP documents transport, selection, actual descriptors and lifecycle' {
    $global:LASTEXITCODE = $safCode
    $safOut
  } @('SafDocumentsContractTest passed:', 'nativeFiles=true', 'SAF_BINDER_NATIVE peer credential provenance verified')
  $detachedUiOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
    'DetachedNativeUiContractTest') 45
  $detachedUiCode = $LASTEXITCODE
  Check 'services: detached document and chooser retain focused-task coverage' {
    $global:LASTEXITCODE = $detachedUiCode
    $detachedUiOut
  } @('DetachedNativeUiContractTest passed:')
  foreach ($safPhase in @('write', 'read', 'released')) {
    $safRestartOut = FixesRun ($dvmFixes + 'gesher.services.ServiceFixtureRunner' +
      'SafDocumentsContractTest' + $safPhase + (NoDrive $safRestart)) 45
    $safRestartCode = $LASTEXITCODE
    Check "services: SAF durable read/write release across fresh process $safPhase" {
      $global:LASTEXITCODE = $safRestartCode
      $safRestartOut
    } @("SafDocumentsContractTest restart $safPhase passed")
  }
} finally {
  try { Stop-SafBinderFixture $safBinderSession }
  finally {
    if ($safSpoofPresent) { Set-Item Env:GESHER_MICROG_SPOOF $safSpoofValue }
    else { Remove-Item Env:GESHER_MICROG_SPOOF -ErrorAction SilentlyContinue }
    Remove-Item $safRestart -Recurse -Force -ErrorAction SilentlyContinue
  }
}

# Real COM dialogs close through their own message-loop lifetime timer; an
# unavailable desktop or failed dialog is a failure, never a passing selection.
$pickerOut = FixesRun ($dvmFixes + 'gesher.services.DocumentPickerTest') 90
$pickerCode = $LASTEXITCODE
Check 'services: real Windows open/save/tree dialogs cancel and retire their resources' {
  $global:LASTEXITCODE = $pickerCode
  $pickerOut
} @('DocumentPickerTest passed: real open/save/tree cancellation and Windows path mapping')

# android.graphics.ColorMatrix on the boot class path (framework.jar lacks it), and
# FileChannel locks over FileDispatcherImpl.lock0/release0 (fcntl record locks).
$fixesOut = FixesRun ($dvmFixes + 'gesher.services.FrameworkFixesTest') 90
$fixesCode = $LASTEXITCODE
Check 'services: android.graphics.ColorMatrix is on the boot class path and computes' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: ColorMatrix on the boot class path: identity, setSaturation, setScale, postConcat',
  'FrameworkFixesTest all passed')
Check 'services: FileChannel lock, tryLock and release (FileDispatcherImpl over fcntl record locks)' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: FileChannel lock, tryLock (shared, ranges) and release',
  'FrameworkFixesTest all passed')
Check 'services: android.view.RenderNode and DisplayListCanvas (pre-API-29 names) for older UI libraries' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: android.view.RenderNode and DisplayListCanvas with create, start, end, drawRenderNode',
  'FrameworkFixesTest all passed')
Check 'services: android.net.nsd.NsdManager (the Connectivity module''s API) on the boot class path' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: android.net.nsd.NsdManager: discovery starts, registration fails through its listener, unregistering does not throw',
  'FrameworkFixesTest all passed')
Check 'services: a split APK install takes its native libraries and ABI from the ABI split (config.arm64_v8a.apk)' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: NativeLibs: the package ABI and libraries come from its ABI split (config.arm64_v8a.apk), not base.apk alone',
  'FrameworkFixesTest all passed')
Check 'services: a stand-in service answers an empty list, not null, for a list method it does not implement' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: a stand-in''s unimplemented list method answers an empty list (IShortcutService.getShortcuts), not null',
  'FrameworkFixesTest all passed')
Check 'services: android.text.Html.fromHtml parses with tagsoup (the ext boot jar''s classes in gesher-app.jar)' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: android.text.Html.fromHtml parses with tagsoup (the ext boot jar''s): text, entities, a bold span and a link',
  'FrameworkFixesTest all passed')
Check 'services: android.net.DnsResolver (the Connectivity module''s API) resolves through Windows' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: android.net.DnsResolver: query and rawQuery answer localhost through the executor, other types NOTIMP, a cancelled query is silent',
  'FrameworkFixesTest all passed')
Check 'services: BluetoothManager without an adapter, and android.os.Debug''s native heap counters' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: BluetoothManager without an adapter; Debug.getNativeHeapSize, getNativeHeapAllocatedSize and getNativeHeapFreeSize',
  'FrameworkFixesTest all passed')
Check 'services: BLE callback subclasses link without an adapter; scan records and device/result parcels work' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: BLE callback subclass links without an adapter; AOSP advertisement parsing and ScanResult/device parcels round trip',
  'FrameworkFixesTest all passed')
Check 'services: Binder Debug counters use actual native objects and report unavailable transaction totals' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: Binder Debug counters track native local objects and Parcel release; proxy/death counts resolve, transaction totals unavailable',
  'FrameworkFixesTest all passed')
Check 'services: android.os.Debug memory accounting (getMemoryInfo, getPss, getRss, getMemInfo) from the Windows working set' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: Debug.getMemoryInfo, getPss, getRss and getMemInfo report the process working set and system memory',
  'FrameworkFixesTest all passed')
Check 'services: libphonenumber (the ext boot jar''s, in framework.jar with its metadata) parses and formats numbers' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: libphonenumber on the boot class path: parse, format (E164, INTERNATIONAL, NATIONAL), findNumbers and PhoneNumberUtils.formatNumber',
  'FrameworkFixesTest all passed')

Check 'services: HandlerThread.quit with tracing off (MessageQueue''s Perfetto counter event; Gmail''s emojiCompat thread)' {
  $global:LASTEXITCODE = $fixesCode
  $fixesOut
} @('FrameworkFixesTest: HandlerThread.quit after a message, and every PerfettoTrackEventExtra class initializes with tracing off',
  'FrameworkFixesTest all passed')

# These module value classes are packaged in gesher-app.jar. Run each fixture in
# its own bounded ART process, preserving its exit status as well as its marker.
foreach ($fixture in @(
  @('DomainVerificationContractTest', 'domain verification empty records and honest mutation results'),
  @('GmsSetupContractTest', 'GMS foreground idle accounting and reserved account visibility policy'),
  @('NdefDataContractTest', 'NDEF bytes, helpers, malformed input and native Parcel round trips'),
  @('PowerWakeLockCallbackTest', 'wake lock listener callbacks, real held state and token lifetime'),
  @('PackageManifestFlagsTest', 'complete parsed manifest flags preserve installed application state'),
  @('AppStorageContextTest', 'distinct CE/DE contexts, files, preferences, databases and data migration'),
  @('PackageServiceQueriesTest', 'package UID/group queries and Android 16 dex-module completion'),
  @('BluetoothValueContractTest', 'BLE settings, filters, advertisements and parcels without an adapter'),
  @('MediaStoreContractTest', 'media collections, resolver forwarding and unavailable-provider handling'),
  @('NetworkRequestValueContractTest', 'DHCP, L2CAP, DNS-SD and network request filters and parcels'),
  @('NetworkValueContractTest', 'MAC, static routes, IP and proxy value contracts'),
  @('ProxyInfoContractTest', 'nullable proxy snapshots and PAC initialization before a local port exists')
)) {
  $valueOut = FixesRun ($dvmFixes + "gesher.services.$($fixture[0])") 45
  $valueCode = $LASTEXITCODE
  Check "services: $($fixture[1])" {
    $global:LASTEXITCODE = $valueCode
    $valueOut
  } @("$($fixture[0]) all passed:")
}

# org.apache.http.legacy (external/apache-http), a shared library apps name in
# <uses-library>, not on the boot class path: laid out as gesher-app.ps1 installs it
# ($ANDROID_ROOT/framework and etc/permissions, the declaration Gesher ships), in a
# system root of its own so the other tracks' apps do not get it implicitly.
$httpRoot = Join-Path $root 'http-legacy-system'
New-Item -ItemType Directory -Force (Join-Path $httpRoot 'framework'), (Join-Path $httpRoot 'etc/permissions') | Out-Null
Copy-Item "$Dir/org.apache.http.legacy.jar" (Join-Path $httpRoot 'framework') -Force
Copy-Item (Join-Path $PSScriptRoot '../../../tools/package/etc/permissions/org.apache.http.legacy.xml') (Join-Path $httpRoot 'etc/permissions') -Force
$httpOut = FixesRun ($dvmFixes + 'gesher.services.HttpLegacyTest' + (NoDrive $httpRoot)) 90
$httpCode = $LASTEXITCODE
Check 'services: org.apache.http.legacy resolves from its declaration to the system root''s jar' {
  $global:LASTEXITCODE = $httpCode
  $httpOut
} @('HttpLegacyTest: org.apache.http.legacy resolves to $ANDROID_ROOT/framework/org.apache.http.legacy.jar',
  'HttpLegacyTest all passed')
Check 'services: apps targeting API 27 or lower get org.apache.http.legacy implicitly' {
  $global:LASTEXITCODE = $httpCode
  $httpOut
} @('HttpLegacyTest: apps targeting API 27 or lower get it implicitly, API 28 and up only by name',
  'HttpLegacyTest all passed')
Check 'services: BasicHttpParams (Gmail''s account setup) loads from org.apache.http.legacy.jar' {
  $global:LASTEXITCODE = $httpCode
  $httpOut
} @('HttpLegacyTest: BasicHttpParams, HttpConnectionParams and HttpGet load from the jar, not the boot class path',
  'HttpLegacyTest all passed')
Check 'services: BasicCookieStore (Gmail''s contacts client) loads and works from org.apache.http.legacy.jar' {
  $global:LASTEXITCODE = $httpCode
  $httpOut
} @('HttpLegacyTest: BasicCookieStore loads from the shared jar and stores and clears a synthetic cookie',
  'HttpLegacyTest all passed')

# An installed APK's signers (ApkPackage), which apps read for their own: WhatsApp's
# registration token is built from GET_SIGNATURES. Its fixtures (tests/dex-services/
# fixtures, generate.sh) are signed as WhatsApp is: a key rotated for SDK 33 and up.
$signingOut = FixesRun ($dvmFixes + 'gesher.services.SigningTest' + (NoDrive (Join-Path $PSScriptRoot '../../dex-services/fixtures'))) 90
$signingCode = $LASTEXITCODE
Check 'services: GET_SIGNATURES answers a rotated APK''s oldest key, as WhatsApp''s token needs' {
  $global:LASTEXITCODE = $signingCode
  $signingOut
} @('SigningTest: GET_SIGNATURES answers a rotated APK''s oldest key',
  'SigningTest: GET_SIGNING_CERTIFICATES answers the current key and its rotation history',
  'SigningTest all passed')
Check 'services: a v2-signed APK answers its key, an unsigned one none' {
  $global:LASTEXITCODE = $signingCode
  $signingOut
} @('SigningTest: a v2-signed APK answers its key, an unsigned one none',
  'SigningTest all passed')

$recognitionBrokerOut = FixesRun ($dvmFixes + 'com.gesher.server.SpeechRecognitionContractTest') 45
$recognitionBrokerCode = $LASTEXITCODE
Check 'services: speech provider ownership, lifecycle, support and download callbacks' {
  $global:LASTEXITCODE = $recognitionBrokerCode
  $recognitionBrokerOut
} @('SpeechRecognitionContractTest all passed:')

$recognizerClientOut = FixesRun ($dvmFixes + 'gesher.services.SpeechRecognizerConnectionTest') 45
$recognizerClientCode = $LASTEXITCODE
Check 'framework: SpeechRecognizer connection callback and cancellation contracts' {
  $global:LASTEXITCODE = $recognizerClientCode
  $recognizerClientOut
} @('SpeechRecognizerConnectionTest all passed:')
