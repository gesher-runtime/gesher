# microG / Google Play Services compatibility track (GMS, FCM, Location, Auth, Synthetic Packages).
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

GmsCheck 'microg-gms (single process): GoogleApiAvailability stand-ins' $outSingle $codeSingle @(
  'gms: GoogleApiAvailability: isGooglePlayServicesAvailable SUCCESS (0)',
  'gms: GoogleApiAvailability: makeGooglePlayServicesAvailable completed'
)

GmsCheck 'microg-gms (single process): PackageManager synthetic GMS & Vending packages' $outSingle $codeSingle @(
  'gms: PackageManager: com.google.android.gms synthetic package present, enabled, valid signatures (Google''s certificate)',
  'gms: PackageManager: com.android.vending synthetic package present, enabled'
)

GmsCheck 'microg-gms (single process): FusedLocationProviderClient connected to LocationManagerService' $outSingle $codeSingle @(
  'gms: FusedLocationProviderClient: connected to LocationManagerService, updates received'
)

GmsCheck 'microg-gms (single process): GoogleSignIn & AccountManager dummy accounts' $outSingle $codeSingle @(
  'gms: GoogleSignIn: dummy account user@gesher.local signed in with idToken',
  'gms: AccountManager: com.google account and dummy auth token returned'
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
  'gms: GoogleApiAvailability: isGooglePlayServicesAvailable SUCCESS (0)',
  'gms: PackageManager: com.google.android.gms synthetic package present, enabled, valid signatures (Google''s certificate)',
  'gms: FusedLocationProviderClient: connected to LocationManagerService, updates received',
  'gms: GoogleSignIn: dummy account user@gesher.local signed in with idToken',
  'gms: FirebaseMessaging: token obtained, com.google.android.c2dm.intent.RECEIVE routed and received',
  'gms: all steps ran'
)
