# Offline microG regressions. Live Google Check-in/FCM is a separate, opt-in test.
# Optional GESHER_MICROG_TEST_APK points to an unmodified official GmsCore APK.
# Dot-sourced after track 50 (CheckTimed); bcpApp includes gesher-app.jar.
if (Test-Path "$Dir/com.android.location.provider.jar") {
    New-Item -ItemType Directory -Force "$root/system/etc/permissions" | Out-Null
    Copy-Item "$Dir/com.android.location.provider.jar" "$root/system/framework" -Force
    Copy-Item "$PSScriptRoot/../../../tools/package/etc/permissions/com.android.location.provider.xml" `
        "$root/system/etc/permissions" -Force
}
$microgVm = @(
    "-Xbootclasspath:$bcpApp",
    "-Ximage:$(NoDrive "$root/system/framework/boot.art")",
    '-Xint', '-Xusejit:false', '-cp', (NoDrive "$Dir/services-tests.dex"),
    'gesher.services.MicrogTest',
    (NoDrive (Join-Path $PSScriptRoot '../../dex-services/fixtures'))
)
if ($env:GESHER_MICROG_TEST_APK) {
    $microgVm += NoDrive $env:GESHER_MICROG_TEST_APK
    if ($env:GESHER_MICROG_TEST_TAMPERED_APK -or $env:GESHER_MICROG_TEST_COMPANION_APK -or $env:GESHER_MICROG_TEST_GSF_APK) {
        $microgVm += if ($env:GESHER_MICROG_TEST_TAMPERED_APK) { NoDrive $env:GESHER_MICROG_TEST_TAMPERED_APK } else { '' }
    }
    if ($env:GESHER_MICROG_TEST_COMPANION_APK -or $env:GESHER_MICROG_TEST_GSF_APK) {
        $microgVm += if ($env:GESHER_MICROG_TEST_COMPANION_APK) { NoDrive $env:GESHER_MICROG_TEST_COMPANION_APK } else { '' }
    }
    if ($env:GESHER_MICROG_TEST_GSF_APK) { $microgVm += NoDrive $env:GESHER_MICROG_TEST_GSF_APK }
}
$microgPrevious = $env:GESHER_MICROG_SPOOF
try {
    foreach ($mode in '0', '1') {
        $env:GESHER_MICROG_SPOOF = $mode
        CheckTimed "microg: offline signer, fallback and opt-in checks (enabled=$mode)" `
            "$Dir/dalvikvm.exe" $microgVm @('MicrogTest all passed') 120
    }
    if ($env:GESHER_MICROG_TEST_APK) {
        $consentVm = @(
            "-Xbootclasspath:$bcpApp", "-Ximage:$(NoDrive "$root/system/framework/boot.art")",
            '-Xint', '-Xusejit:false', '-cp', (NoDrive "$Dir/services-tests.dex"),
            'gesher.services.ServiceFixtureRunner', 'PermissionRequestsTest', (NoDrive $env:GESHER_MICROG_TEST_APK)
        )
        CheckTimed 'microg: runtime consent grant/deny and replacement race (injected test decisions)' `
            "$Dir/dalvikvm.exe" $consentVm @('PermissionRequestsTest: async grant/deny, result mapping and replacement race passed') 120
    }
} finally {
    $env:GESHER_MICROG_SPOOF = $microgPrevious
}

# Real app Binder identities, native credential sealing/restart and live
# permission invalidation run without installing or executing any Google APK.
$securityArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
    "$PSScriptRoot/../run-microg-security.ps1", '-Dir', $Dir)
if ($env:GESHER_MICROG_TEST_APK) {
    $securityArgs += @('-GmsCoreApk', $env:GESHER_MICROG_TEST_APK, '-RequireMetadataSuites')
}
CheckTimed 'microg: isolated account, identity and permission integration' `
    'powershell.exe' $securityArgs @('PASS  offline microG security integration complete') 900 -Publish
