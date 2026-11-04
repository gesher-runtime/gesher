# PROTOTYPE: End-to-End Windows Test Track for microG Integration on Gesher
# Track 52: Verifies signature spoofing gating, GmsCore/FakeStore package resolution,
# Chimera & Auth ContentProviders, AccountAuthenticator registration, and C2DM/FCM broadcast routing.

. "$PSScriptRoot/../helpers/CloseProcessReaders.ps1"

$dalvikvm = Join-Path $Dir 'dalvikvm.exe'
$dvm = @(
    "-Xbootclasspath:$bcp",
    "-Ximage:$(NoDrive "$Dir/framework/boot.art")",
    '-Xint',
    '-Xusejit:false'
)

# 1. Verify synthetic GMS fallback when microG APK is NOT installed
$env:GESHER_MICROG_SPOOF = "0"
CheckTimed "microg-synthetic-fallback" $dalvikvm ($dvm + @(
    "com.gesher.server.RuntimeCommands",
    "verify-gms-fallback"
)) @(
    "gms.synthetic=true",
    "gms.signature.match=true"
)

# 2. Verify signature spoofing is denied to non-allowlisted packages even when GESHER_MICROG_SPOOF=1
$env:GESHER_MICROG_SPOOF = "1"
CheckTimed "microg-spoof-allowlist-enforcement" $dalvikvm ($dvm + @(
    "com.gesher.server.RuntimeCommands",
    "verify-spoof-gate",
    "com.example.untrusted"
)) @(
    "spoof.allowed=false",
    "permission.FAKE_PACKAGE_SIGNATURE=DENIED"
)

# 3. Verify signature spoofing is granted to allowlisted com.google.android.gms when enabled
CheckTimed "microg-spoof-gmscore-granted" $dalvikvm ($dvm + @(
    "com.gesher.server.RuntimeCommands",
    "verify-spoof-gate",
    "com.google.android.gms"
)) @(
    "spoof.allowed=true",
    "permission.FAKE_PACKAGE_SIGNATURE=GRANTED",
    "signature.sha1=38918a453d07199354f8b19af05ec6562ced5788"
)

# 4. Optional full APK track (runs when microG GmsCore APK is staged under system/priv-app/GmsCore)
$gmsApk = Join-Path $root "system/priv-app/GmsCore/com.google.android.gms.apk"
if (Test-Path $gmsApk) {
    CheckTimed "microg-gmscore-providers-and-auth" $dalvikvm ($dvm + @(
        "com.gesher.server.RuntimeCommands",
        "verify-microg-apk",
        (NoDrive $gmsApk)
    )) @(
        "provider.chimera=org.microg.gms.chimera.ServiceProvider",
        "provider.auth.accounts=org.microg.gms.auth.AccountContentProvider",
        "provider.gservices=org.microg.gms.gservices.GServicesProvider",
        "authenticator.google=org.microg.gms.auth.loginservice.GoogleLoginService",
        "service.clearcut=org.microg.gms.clearcut.ClearcutLoggerService",
        "service.mcs=org.microg.gms.gcm.McsService"
    )
} else {
    Write-Host "SKIP  microg-gmscore-providers-and-auth (GmsCore APK not staged at $gmsApk)"
}
