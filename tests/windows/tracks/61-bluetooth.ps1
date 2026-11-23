# Bluetooth (docs/bluetooth.md, ADR 0085): the android.bluetooth classes over the host
# radio. Dot-sourced by run-tests.ps1, using Check, NoDrive, $Dir, $root and $failed from
# there and RunTimed from 50-services.ps1. The runners have no radio: one run checks
# that there is no adapter (and logs what Windows answered); the other runs every path
# above the JNI layer over GESHER_BLUETOOTH_TEST's radio (compat/devices/bluetooth_test.cc).

Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework') -Force
Copy-Item "$Dir/build.prop" (Join-Path $root 'system') -Force
$bcpBt = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmBt = @("-Xbootclasspath:${bcpBt}:$(NoDrive "$Dir/services-tests.dex")", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false',
  'gesher.services.BluetoothHostTest')

# Server-local fixture injection keeps request policy deterministic, including on
# machines with a real adapter. This never opens consent UI or changes its radio.
$requestOut = RunTimed "$Dir/dalvikvm.exe" @("-Xbootclasspath:${bcpBt}:$(NoDrive "$Dir/services-tests.dex")",
  "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false',
  'gesher.services.BluetoothRequestIntentTest') 120
$requestCode = $LASTEXITCODE
Check 'bluetooth: request intents preserve consent, permissions, observed state and bounded asynchronous completion' {
  $global:LASTEXITCODE = $requestCode
  $requestOut
} @('BluetoothRequestIntentTest all passed:')

$hadTest = Test-Path Env:GESHER_BLUETOOTH_TEST
$savedTest = $env:GESHER_BLUETOOTH_TEST
try {
  if ($hadTest) { Remove-Item Env:GESHER_BLUETOOTH_TEST }
  $noRadioOut = RunTimed "$Dir/dalvikvm.exe" ($dvmBt + '--no-radio') 120
  $noRadioCode = $LASTEXITCODE
  Check 'bluetooth: without a radio there is no adapter (Windows.Devices.Bluetooth answers no default adapter)' {
    $global:LASTEXITCODE = $noRadioCode
    $noRadioOut
  } @('BluetoothHostTest: Windows: no adapter', 'BluetoothHostTest: features: bluetooth false, bluetooth_le false',
    'BluetoothHostTest no radio passed:')

  $env:GESHER_BLUETOOTH_TEST = '1'
  $btOut = RunTimed "$Dir/dalvikvm.exe" $dvmBt 240
  $btCode = $LASTEXITCODE
} finally {
  if ($hadTest) { $env:GESHER_BLUETOOTH_TEST = $savedTest } else { Remove-Item Env:GESHER_BLUETOOTH_TEST -ErrorAction SilentlyContinue }
}

# Every section's line, in the log (each Check also needs "all passed", which the test
# prints only when every assertion of every section held).
($btOut -split "`n" | Where-Object { $_ -match '^BluetoothHostTest' }) | ForEach-Object { Write-Host $_ }
foreach ($c in @(
  @('system features: FEATURE_BLUETOOTH and FEATURE_BLUETOOTH_LE from the radio', 'BluetoothHostTest: features: bluetooth true, bluetooth_le true'),
  @('permissions: BLUETOOTH_CONNECT/BLUETOOTH_SCAN on API 31+, legacy BLUETOOTH below', 'BluetoothHostTest: permissions:'),
  @('gesher.bluetooth checks Binder callers (radio, discovery, SDP, pairing) and asks the user', 'BluetoothHostTest: service policy:'),
  @('adapter state, name, hidden address and the bonded headset (class, service classes, isConnected)', 'BluetoothHostTest: adapter:'),
  @('A2DP and headset profile proxies list the connected headset', 'BluetoothHostTest: profiles:'),
  @('LE scan: filters, advertisement merged with its scan response, legacy startLeScan', 'BluetoothHostTest: LE scan:'),
  @('GATT client: connect, discovery, read, CCCD notifications, write and echo, MTU, disconnect', 'BluetoothHostTest: GATT:'),
  @('GATT: included secondary services, read by UUID, PHY reported and declined, two clients of one device',
    'an included secondary one, read 87 (by UUID too), notifications through the CCCD, write/echo, MTU 247, PHY, two clients'),
  @('RFCOMM: service UUID and channel sockets, available(), listener accept', 'BluetoothHostTest: RFCOMM:'),
  @('RFCOMM: isEncrypted only for a secure socket, close() cancels a blocked connect', 'encrypted secure link, close() cancels a connect'),
  @('system broadcasts: radio off/on, ACL and A2DP state, discovery, ACTION_UUID', 'BluetoothHostTest: broadcasts:'),
  @('pairing through Windows: createBond BONDING>BONDED, removeBond BONDED>NONE, a failed pairing', 'BluetoothHostTest: pairing:')
)) {
  Check "bluetooth (test radio): $($c[0])" {
    $global:LASTEXITCODE = $btCode
    $btOut
  } @($c[1], 'BluetoothHostTest all passed:')
}
