# Google client-library fixture track without microG (GMS availability, FCM, Location, Auth).
# Dot-sourced by run-tests.ps1, using Check, NoDrive, $Dir, $root, $bcpApp and $failed from there.

if (-not (Test-Path "$Dir/gmsapp.apk")) {
  Write-Host 'SKIP  microg-gms: gmsapp.apk not present in build output'
  return
}

$dvmGms = @(
  "-Xbootclasspath:$bcpApp",  # framework.jar and gesher-app.jar: com.gesher.app.Main / Launcher
  "-Ximage:$(NoDrive $root)/system/framework/boot.art",
  '-Xint',
  '-Xusejit:false'
)

function GmsCheck([string] $Name, [string] $Out, [int] $Code, [string[]] $Expect) {
  $missing = $Expect | Where-Object { -not $Out.Contains($_) }
  if ($Code -eq 0 -and -not $missing) {
    Write-Host "PASS  $Name"
  } else {
    Write-Host "FAIL  $Name (exit $Code; missing: $($missing -join ', '))"
    Write-Host $Out
    $script:failed++
  }
}

# Single process execution: app driver runs with in-process stand-in services.
$outSingle = RunTimed "$Dir/dalvikvm.exe" ($dvmGms + 'com.gesher.app.Main' + (NoDrive "$Dir/gmsapp.apk")) 300
$codeSingle = $LASTEXITCODE

# The client-SDK stubs are the APK's own classes (tests/apps/gmsapp/build.sh bundles
# them); gesher-app.jar on the boot class path has none (app/build.sh), or they would
# shadow an app's real Firebase / play-services (parent-first loading).
GmsCheck 'microg-gms (single process): client SDK classes come from the APK, none from the boot class path' $outSingle $codeSingle @(
  'gms: client SDK: FirebaseApp and play-services classes loaded from the APK, none on the boot class path'
)

# Without microG there is no Play services package (#335): no package context for the
# client library's ProviderInstaller, SERVICE_MISSING, and neither package is listed.
GmsCheck 'microg-gms (single process): no Play services package context without microG' $outSingle $codeSingle @(
  'gms: ProviderInstaller: no Play services package context without Play services (NameNotFoundException)'
)

GmsCheck 'microg-gms (single process): GoogleApiAvailability reports SERVICE_MISSING' $outSingle $codeSingle @(
  'gms: GoogleApiAvailability: isGooglePlayServicesAvailable SERVICE_MISSING (1)',
  'gms: GoogleApiAvailability: makeGooglePlayServicesAvailable failed without Play services'
)

GmsCheck 'microg-gms (single process): PackageManager has no GMS or Vending without microG' $outSingle $codeSingle @(
  'gms: PackageManager: com.google.android.gms and com.android.vending absent without microG'
)

GmsCheck 'microg-gms (single process): FusedLocationProviderClient connected to LocationManagerService' $outSingle $codeSingle @(
  'gms: FusedLocationProviderClient: connected to LocationManagerService, updates received'
)

GmsCheck 'microg-gms (single process): no Google account or token without microG' $outSingle $codeSingle @(
  'gms: GoogleSignIn: no signed-in account without Play services (SERVICE_MISSING)',
  'gms: AccountManager: no com.google account; getAuthToken fails without an authenticator'
)

GmsCheck 'microg-gms (single process): FirebaseMessaging & FCM receiver intent routing' $outSingle $codeSingle @(
  'gms: FirebaseMessaging: token obtained, com.google.android.c2dm.intent.RECEIVE routed and received',
  'gms: all steps ran'
)

# Multi-process (system server device) execution: services in system_server, app across Binder.
$launcherGms = $dvmGms + 'com.gesher.app.Launcher'
$null = RunTimed "$Dir/dalvikvm.exe" ($launcherGms + 'shutdown') 120
$outDevice = RunTimed "$Dir/dalvikvm.exe" ($launcherGms + 'launch' + (NoDrive "$Dir/gmsapp.apk") + '--keep-task') 400
$codeDevice = $LASTEXITCODE

$serverLog = Join-Path $root 'logs/system_server.log'
$appLog = Join-Path $root 'logs/com.gesher.gmsapp.log'
$outDevice += "`n" + (Get-Content $serverLog -Raw -ErrorAction SilentlyContinue)
$outDevice += "`n" + (Get-Content $appLog -Raw -ErrorAction SilentlyContinue)

GmsCheck 'microg-gms (device): full GMS compatibility suite in system server' $outDevice $codeDevice @(
  'gms: client SDK: FirebaseApp and play-services classes loaded from the APK, none on the boot class path',
  'gms: ProviderInstaller: no Play services package context without Play services (NameNotFoundException)',
  'gms: GoogleApiAvailability: isGooglePlayServicesAvailable SERVICE_MISSING (1)',
  'gms: PackageManager: com.google.android.gms and com.android.vending absent without microG',
  'gms: FusedLocationProviderClient: connected to LocationManagerService, updates received',
  'gms: GoogleSignIn: no signed-in account without Play services (SERVICE_MISSING)',
  'gms: AccountManager: no com.google account; getAuthToken fails without an authenticator',
  'gms: FirebaseMessaging: token obtained, com.google.android.c2dm.intent.RECEIVE routed and received',
  'gms: all steps ran'
)

# The device was kept up (--keep-task): shut it down, or its processes keep
# system/framework/framework-res.apk mapped and the next track cannot replace it.
$null = RunTimed "$Dir/dalvikvm.exe" ($launcherGms + 'shutdown') 120
