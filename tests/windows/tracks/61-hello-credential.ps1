# Device-credential confirmation (ADR 0091, phase 2b): what an ordinary app
# (tests/apps/hellocred) is answered by BiometricManager, BiometricPrompt,
# KeyguardManager and the confirm-credential intent, across Binder on the Gesher
# device. Dot-sourced by run-tests.ps1, using NoDrive, $Dir, $root and $failed from
# there and DevRun/DevCheck from 58-devices.ps1.
#
# The runners have no Windows Hello set up (and Windows Server 2022 is below build
# 22000), so the device is not secure and every answer is Android's unavailable one.
# The app is started in its 'answers' mode, which never shows a prompt on a secure
# device: on a machine with Windows Hello this track then fails at the "not secure"
# check instead of waiting for a person. The secure paths are verified by hand
# (HUMAN_TEST.md of the change).

Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework') -Force
Copy-Item "$Dir/build.prop" (Join-Path $root 'system') -Force
$bcpHc = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$launcherHc = @("-Xbootclasspath:$bcpHc", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false',
  'com.gesher.app.Launcher')
$null = DevRun "$Dir/dalvikvm.exe" ($launcherHc + 'shutdown') 120
$hadHcMode = Test-Path Env:GESHER_HELLOCRED_MODE
$savedHcMode = $env:GESHER_HELLOCRED_MODE
try {
  $env:GESHER_HELLOCRED_MODE = 'answers'
  $hcOut = DevRun "$Dir/dalvikvm.exe" ($launcherHc + 'launch', (NoDrive "$Dir/hellocred.apk"), '--keep-task') 300
  $hcCode = $LASTEXITCODE
} finally {
  if ($hadHcMode) { $env:GESHER_HELLOCRED_MODE = $savedHcMode } else { Remove-Item Env:GESHER_HELLOCRED_MODE -ErrorAction SilentlyContinue }
}
$hcServerLog = Get-Content (Join-Path $root 'logs/system_server.log') -Raw -ErrorAction SilentlyContinue
$hcOut += "`n" + (Get-Content (Join-Path $root 'logs/com.gesher.hellocred.log') -Raw -ErrorAction SilentlyContinue)
$null = DevRun "$Dir/dalvikvm.exe" ($launcherHc + 'shutdown') 120

# What Windows answered and what the app saw, in the log.
($hcServerLog -split "`n" | Where-Object { $_ -match 'auth: |confirm-credential: |gesher hello: ' }) | ForEach-Object { Write-Host $_ }
($hcOut -split "`n" | Where-Object { $_ -match '^hellocred: ' } | Select-Object -Unique) | ForEach-Object { Write-Host $_ }

$hc = 'hellocred: '
DevCheck 'hello credential: without Windows Hello the device is not secure and not locked (KeyguardManager.isDeviceSecure, isKeyguardSecure, isDeviceLocked)' $hcOut @(
  ,@("${hc}mode: answers", "${hc}keyguard: isDeviceSecure false, isKeyguardSecure false, isDeviceLocked false, isKeyguardLocked false")) $hcCode 'hellocred'
DevCheck 'hello credential: BiometricManager.canAuthenticate as PreAuthInfo on an unsecured device without sensors (credential NONE_ENROLLED 11, biometrics NO_HARDWARE 12)' $hcOut @(
  ,@("${hc}biometrics: canAuthenticate credential 11, strong 12, weak 12, strong|credential 11, weak|credential 11")) $hcCode 'hellocred'
DevCheck 'hello credential: createConfirmDeviceCredentialIntent is null on a device that is not secure' $hcOut @(
  ,@("${hc}keyguard: createConfirmDeviceCredentialIntent null")) $hcCode 'hellocred'
# A prompt from an app whose window Windows does not have in the foreground is ended
# with BIOMETRIC_ERROR_CANCELED (5), as AuthService.authenticate does for a caller that
# is not the top app; a runner's desktop does not always give the test window the
# foreground. Otherwise: no sensor is HW_NOT_PRESENT (12), no credential is
# NO_DEVICE_CREDENTIAL (14). Never a success.
$hcStrong = if ($hcOut -match 'hellocred: prompt: strong: error (\d+)') { $Matches[1] } else { '?' }
$hcCredential = if ($hcOut -match 'hellocred: prompt: credential: error (\d+)') { $Matches[1] } else { '?' }
if (($hcStrong -eq '12' -and $hcCredential -eq '14') -or ($hcStrong -eq '5' -and $hcCredential -eq '5')) {
  Write-Host "PASS  hello credential: BiometricPrompt ends with Android's unavailable errors (biometric-only $hcStrong, credential $hcCredential), no success"
} else {
  Write-Host "FAIL  hello credential: BiometricPrompt errors: biometric-only $hcStrong (12, or 5 in the background), credential $hcCredential (14, or 5 in the background)"
  $script:failed++
}
if ($hcOut.Contains("${hc}prompt: strong: succeeded") -or $hcOut.Contains("${hc}prompt: credential: succeeded") -or
    $hcOut.Contains("${hc}confirm: result RESULT_OK")) {
  Write-Host 'FAIL  hello credential: a confirmation was reported on a device that is not secure'
  $script:failed++
} else {
  Write-Host 'PASS  hello credential: nothing is reported as confirmed without Windows Hello'
}
DevCheck 'hello credential: ACTION_CONFIRM_DEVICE_CREDENTIAL with no credential to confirm answers RESULT_CANCELED; the factory-reset variant is not resolved' $hcOut @(
  ,@("${hc}confirm: starting the action directly (the device is not secure)", "${hc}confirm: result RESULT_CANCELED",
    "${hc}confirm: FRP ActivityNotFoundException", "${hc}done")) $hcCode 'hellocred'
