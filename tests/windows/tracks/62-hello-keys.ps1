# AndroidKeyStore keys that require the device credential on the combined Windows Hello
# head (ADR 0094 + ADR 0095): what an ordinary app (tests/apps/hellokeys) is answered when
# it makes such keys, across Binder and gesher.keystore on the Gesher device. Dot-sourced
# by run-tests.ps1, using NoDrive, $Dir, $root and $failed from there and DevRun/DevCheck
# from 58-devices.ps1.
#
# The runners have no Windows Hello set up (and Windows Server 2022 is below build
# 22000), so the device is not secure: both keys must be refused exactly as Android
# refuses them without a secure lock screen, while a key without the requirement still
# works. The app is started in its 'answers' mode, which makes no such key on a secure
# device (the first one would ask for Windows Hello): on a machine with Windows Hello this
# track then fails at the "not secure" check instead of waiting for a person. The secure
# sequence is run by hand (HUMAN_TEST.md of the change).

Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework') -Force
Copy-Item "$Dir/build.prop" (Join-Path $root 'system') -Force
$bcpHk = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$launcherHk = @("-Xbootclasspath:$bcpHk", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false',
  'com.gesher.app.Launcher')
$null = DevRun "$Dir/dalvikvm.exe" ($launcherHk + 'shutdown') 120
$hadHkMode = Test-Path Env:GESHER_HELLOKEYS_MODE
$savedHkMode = $env:GESHER_HELLOKEYS_MODE
try {
  $env:GESHER_HELLOKEYS_MODE = 'answers'
  $hkOut = DevRun "$Dir/dalvikvm.exe" ($launcherHk + 'launch', (NoDrive "$Dir/hellokeys.apk"), '--keep-task') 300
  $hkCode = $LASTEXITCODE
} finally {
  if ($hadHkMode) { $env:GESHER_HELLOKEYS_MODE = $savedHkMode } else { Remove-Item Env:GESHER_HELLOKEYS_MODE -ErrorAction SilentlyContinue }
}
$hkServerLog = Get-Content (Join-Path $root 'logs/system_server.log') -Raw -ErrorAction SilentlyContinue
$hkOut += "`n" + (Get-Content (Join-Path $root 'logs/com.gesher.hellokeys.log') -Raw -ErrorAction SilentlyContinue)
$null = DevRun "$Dir/dalvikvm.exe" ($launcherHk + 'shutdown') 120

($hkServerLog -split "`n" | Where-Object { $_ -match 'auth: |keystore|gesher hello' }) | ForEach-Object { Write-Host $_ }
($hkOut -split "`n" | Where-Object { $_ -match '^hellokeys: ' } | Select-Object -Unique) | ForEach-Object { Write-Host $_ }

$hk = 'hellokeys: '
$hkRefused = 'init InvalidAlgorithmParameterException, cause java.lang.IllegalStateException: Secure lock screen must be enabled to create keys requiring user authentication'
DevCheck 'hello keys: without Windows Hello the device is not secure, and a key without user authentication still works' $hkOut @(
  ,@("${hk}mode: answers", "${hk}keyguard: isDeviceSecure false", "${hk}plain: encrypted 40 bytes")) $hkCode 'hellokeys'
DevCheck 'hello keys: setUserAuthenticationParameters(0, AUTH_DEVICE_CREDENTIAL) is refused as Android refuses it without a secure lock screen (KeyStore2ParameterUtils.getRootSid)' $hkOut @(
  ,@("${hk}per-operation: $hkRefused")) $hkCode 'hellokeys'
DevCheck 'hello keys: a time-bound key (30 s, AUTH_DEVICE_CREDENTIAL) is refused the same way' $hkOut @(
  ,@("${hk}time-bound: $hkRefused", "${hk}done")) $hkCode 'hellokeys'
if ($hkOut.Contains("${hk}per-operation: generated") -or $hkOut.Contains("${hk}time-bound: generated") -or
    $hkOut.Contains("${hk}prompt: crypto: succeeded")) {
  Write-Host 'FAIL  hello keys: a key requiring authentication was made, or a confirmation reported, on a device that is not secure'
  $script:failed++
} else {
  Write-Host 'PASS  hello keys: no key requiring authentication and no confirmation without Windows Hello'
}
