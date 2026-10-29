# Devices (docs/devices.md, ADR 0043): location over Windows' location service,
# sensors and cameras. Dot-sourced by run-tests.ps1, whose Check, NoDrive, $Dir,
# $root, $bcp and $failed these use; devices.apk (tests/apps/devices) runs in
# the single-process mode and on the Gesher device.

# Runs a program with a time limit, killing it and what it started on time-out.
function DevRun([string] $Exe, [string[]] $Arguments = @(), [int] $Seconds = 300) {
  $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
  foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  # The 2022 shutdown crash can exit inside ART's fatal handler. Keep its
  # bounded pre-handler context in the failing process's actual diagnostic.
  $psi.Environment['GESHER_TRACE_INVALID_SENTINEL'] = '1'
  $p = [Diagnostics.Process]::Start($psi)
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEndAsync()
  $timedOut = -not $p.WaitForExit($Seconds * 1000)
  if ($timedOut) {
    $p.Kill($true)
    $p.WaitForExit()
  }
  $null = $stdout.Wait(10000), $stderr.Wait(10000)
  $out = $stdout.Result + $stderr.Result
  if ($timedOut) { $out += "`n(timed out after $Seconds s)" }
  if ($out.Contains('invalid sentinel before signal handler')) {
    $symbolizer = Join-Path $PSScriptRoot '../../../tools/symbolize.py'
    if ((Test-Path "$Dir/dalvikvm.map") -and (Test-Path -LiteralPath $symbolizer)) {
      $resolved = @($out | python $symbolizer "$Dir/dalvikvm.map")
      if ($LASTEXITCODE -eq 0) { $out = $resolved -join "`n" }
    }
    # Preserve the bounded context before DevCheck's category/200-line filter.
    # That filter otherwise hides the native fault's RIP and registers.
    ($out -split "`n" | Where-Object { $_ -match '^gesher-signal:' } |
      Select-Object -First 96) | ForEach-Object { Write-Host $_ }
  }
  $global:LASTEXITCODE = if ($timedOut) { -1 } else { $p.ExitCode }
  return $out
}

# Passes when each sequence's lines are in $Out in that order; prints $Out once on failure.
$script:devPrinted = @{}
function DevCheck([string] $Name, [string] $Out, [object[]] $Sequences, [int] $Code = 0, [string] $Tag = '') {
  $missing = @()
  foreach ($seq in $Sequences) {
    $at = 0
    foreach ($e in $seq) {
      $i = $Out.IndexOf($e, $at)
      if ($i -lt 0) { $missing += $e } else { $at = $i + $e.Length }
    }
  }
  if ($Code -eq 0 -and -not $missing) {
    Write-Host "PASS  $Name"
    return
  }
  Write-Host "FAIL  $Name (exit $Code; missing or out of order: $($missing -join ' | '))"
  $script:failed++
  if ($Tag -and -not $script:devPrinted[$Tag]) {
    $script:devPrinted[$Tag] = $true
    ($Out -split "`n" | Where-Object { $_ -match 'devices|location|camera|sensors|gesher-app|system_server|Exception|Error|FATAL| at ' } |
      Select-Object -First 200) | Out-Host
  }
}

Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework') -Force
Copy-Item "$Dir/build.prop" (Join-Path $root 'system') -Force
$bcpDev = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmDev = @("-Xbootclasspath:$bcpDev", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
Check 'devices: camera request selection is atomic against cancel and flush' {
  DevRun "$Dir/dalvikvm.exe" ($dvmDev + '-cp', (NoDrive "$Dir/services-tests.dex"),
    'gesher.services.CameraRequestRaceTest') 90
} @('CameraRequestRaceTest: 1000 contended cancel/flush selections, ordered frame numbers, queued priority and disconnected stop passed')
$d = 'devices: location: '

# The location checks of devices.apk's LocationSteps (both modes).
function LocationChecks([string] $Mode, [string] $Out, [int] $Code, [bool] $PendingIntentReceived) {
  DevCheck "devices: location ($Mode): LocationManager from framework-location, the gps/network/fused/passive providers, no geocoder" $Out @(
    ,@("${d}providers [passive, network, fused, gps]", 'Geocoder.isPresent false',
      "${d}test gps added, enabled false", "${d}test gps enabled true")) $Code $Mode
  $pi = if ($PendingIntentReceived) {
    "${d}update to the PendingIntent pending intent gps 31.7767,35.2345 acc 5 mock true"
  } else {
    # Single process: the PendingIntent's broadcast is sent (no registered receiver
    # there: the process is not attached to a system server).
    'broadcastIntent act=com.gesher.devices.LOCATION'
  }
  DevCheck "devices: location ($Mode): a test provider in gps' place: requestLocationUpdates to a listener and a PendingIntent, getLastKnownLocation, getCurrentLocation, removeUpdates" $Out @(
    @("${d}update to the listener listener gps 31.7767,35.2345 acc 5 mock true",
      "${d}getLastKnownLocation gps gps 31.7767,35.2345 acc 5 mock true, passive gps 31.7767,35.2345 acc 5 mock true",
      "${d}getCurrentLocation current gps 31.7767,35.2345 acc 5 mock true",
      "${d}after removeUpdates(PendingIntent) listener gps 32.0853,34.7818 acc 5 mock true",
      "${d}and nothing else (nothing within 1000 ms)"),
    @('location: registerLocationPendingIntent gps for com.gesher.devices'), @($pi)) $Code $Mode
  DevCheck "devices: location ($Mode): provider disabled and enabled reach the listener; nothing after removeUpdates(listener)" $Out @(
    ,@("${d}test gps disabled: listener onProviderDisabled gps, enabled false",
      "${d}test gps enabled again: listener onProviderEnabled gps",
      "${d}after removeUpdates(listener) (nothing within 1000 ms)")) $Code $Mode
  DevCheck "devices: location ($Mode): fused as apps without Play services ask (LocationRequest, an Executor), test providers removed, Geocoder throws IOException" $Out @(
    ,@("${d}fused via LocationRequest and an Executor fused fused 29.5581,34.9482 acc 5 mock true",
      "${d}best provider fused", "${d}test providers removed",
      "${d}Geocoder throws IOException (no geocoder)", "${d}Windows network provider: ", "${d}done",
      'devices: all steps ran')) $Code $Mode
  # Windows' own location service: whatever this machine allows, the calls end cleanly.
  if ($Out -match 'location: Windows location: ([^\r\n]*)') { $win = $Matches[1] } else { $win = $null }
  if ($Out -match 'devices: location: Windows network provider: ([^\r\n]*)') { $net = $Matches[1] } else { $net = $null }
  if ($Code -eq 0 -and $win -and $net) {
    Write-Host "PASS  devices: location ($Mode): Windows.Devices.Geolocation answers: access $win; network provider: $net"
  } else {
    Write-Host "FAIL  devices: location ($Mode): Windows.Devices.Geolocation (access: $win; network provider: $net; exit $Code)"
    $script:failed++
  }
}

$sn = 'devices: sensors: '
# Sensors without hardware: the runners have none (Windows.Devices.Sensors' GetDefault
# answers null for each kind), so the list is empty and registering fails cleanly.
function SensorsEmptyCheck([string] $Mode, [string] $Out, [int] $Code) {
  if ($Out -match 'sensors: (accelerometer: [^\r\n]*)') { $win = $Matches[1] } else { $win = '(not logged)' }
  DevCheck "devices: sensors ($Mode): SystemSensorManager over Windows.Devices.Sensors; no sensors here, an empty list, registerListener false ($win)" $Out @(
    ,@("${sn}0 sensors", "${sn}no accelerometer; registerListener answers false", "${sn}no light sensor",
      "${sn}done")) $Code $Mode
}
# GESHER_SENSOR_TEST's sensors, below the framework's natives: the whole path.
function SensorsTestCheck([string] $Mode, [string] $Out, [int] $Code) {
  DevCheck "devices: sensors ($Mode): GESHER_SENSOR_TEST's accelerometer and light sensor through SystemSensorManager: the list, events on a Handler, flush, unregisterListener" $Out @(
    ,@("${sn}2 sensors: Gesher Test Accelerometer (android.sensor.accelerometer), Gesher Test Light Sensor (android.sensor.light)",
      "${sn}accelerometer Gesher Test Accelerometer, range 78.4532, min delay 10000 us",
      "${sn}registerListener(accelerometer) true, first event Gesher Test Accelerometer 0.00,0.00,9.81 accuracy 3",
      "${sn}accelerometer events keep coming: 4 more", "${sn}flush: flush Gesher Test Accelerometer",
      "${sn}after unregisterListener (nothing within 500 ms)",
      "${sn}light event Gesher Test Light Sensor 250.00,0.00,0.00 accuracy 3", "${sn}done")) $Code $Mode
}

$cm = 'devices: camera: '
# Cameras without hardware: Media Foundation finds no capture device on the runners.
function CameraEmptyCheck([string] $Mode, [string] $Out, [int] $Code) {
  if ($Out -match 'camera: ((no )?Media Foundation[^;\r\n]*)') { $mf = $Matches[1] } else { $mf = '(not logged)' }
  DevCheck "devices: camera ($Mode): CameraManager over Media Foundation; no camera here ($mf): an empty id list, openCamera throws, legacy Camera none, no ACTION_IMAGE_CAPTURE activity" $Out @(
    ,@("${cm}0 cameras", "${cm}legacy android.hardware.Camera.getNumberOfCameras() 0",
      "${cm}legacy Camera.open() answers null", "${cm}ACTION_IMAGE_CAPTURE resolves to null",
      "${cm}no cameras; openCamera(`"0`") throws IllegalArgumentException", "${cm}done")) $Code $Mode
}
# GESHER_CAMERA_TEST's camera, below the camera service: Camera2 end to end.
function CameraTestCheck([string] $Mode, [string] $Out, [int] $Code) {
  $before = $script:failed
  DevCheck "devices: camera ($Mode): GESHER_CAMERA_TEST's camera through Camera2: characteristics, openCamera, a session, repeating preview into a YUV ImageReader, a JPEG still capture, close" $Out @(
    ,@("${cm}1 cameras: 0",
      "${cm}0 facing 1, level 4, YUV_420_888 [640x480, 320x240], JPEG [640x480, 320x240]",
      "${cm}openCamera opened 0", "${cm}createCaptureSession configured",
      "${cm}repeating preview: first frame preview 640x480 format 35, 3 planes, Y(10,100)=110, timestamp>0 true",
      "${cm}preview frames keep coming: true, results true",
      "${cm}still capture: capture completed, sensor timestamp>0 true, JPEG quality 90; still JPEG 320x240, starts FFD8 true, decodes to 320x240",
      "${cm}close closed 0", "${cm}done")) $Code "camera $Mode"
  if ($script:failed -gt $before) {
    ($Out -split "`n" | Where-Object { $_ -match 'camera|Camera|Image|still' } | Select-Object -Last 80) | Out-Host
  }
  # The frames came through Media Foundation: its source reader over the test camera's
  # IMFMediaSource (YUY2, converted to NV12 by the reader's video processing).
  $opens = [regex]::Matches($Out, 'camera: opened gesher-test at (\d+x\d+) through ([^\r\n]*)')
  $viaMf = @($opens | Where-Object { $_.Groups[2].Value -like 'Media Foundation (IMFSourceReader over the test media source*' })
  if ($Out -match '(test camera \(GESHER_CAMERA_TEST\)[^;\r\n]*)') { $tc = $Matches[1] } else { $tc = '(not logged)' }
  if ($Code -eq 0 -and $viaMf.Count -gt 0 -and $viaMf.Count -eq $opens.Count) {
    Write-Host "PASS  devices: camera ($Mode): the test camera's frames through Media Foundation: $tc; opened at $(($opens | ForEach-Object { $_.Groups[1].Value }) -join ', ') through $($viaMf[0].Groups[2].Value)"
  } elseif ($Mode -ne 'single process' -and $opens.Count -eq 0) {
    # The Gesher device's app process logs to its own log, not always here.
    Write-Host "SKIP  devices: camera ($Mode): the app process' open log is not in this output"
  } else {
    Write-Host "FAIL  devices: camera ($Mode): the test camera's frames through Media Foundation ($tc; $($opens.Count) opens, $($viaMf.Count) through Media Foundation; exit $Code)"
    $script:failed++
    ($Out -split "`n" | Where-Object { $_ -match 'camera' } | Select-Object -First 60) | Out-Host
  }
}

# The legacy android.hardware.Camera over the same test camera (hwsvc2, ADR 0051).
function LegacyCameraCheck([string] $Mode, [string] $Out, [int] $Code) {
  DevCheck "devices: camera ($Mode): the legacy android.hardware.Camera over GESHER_CAMERA_TEST's camera: info, parameters, a 320x240 NV21 preview into callback buffers, autoFocus, takePicture (shutter, raw, JPEG), release" $Out @(
    ,@("${cm}legacy android.hardware.Camera.getNumberOfCameras() 1", "${cm}legacy Camera.open() answers a camera",
      "${cm}legacy 1 cameras, 0 facing 0, orientation 0", "${cm}legacy open(0) opened",
      "${cm}legacy parameters: preview sizes [640x480, 320x240], preview format 17, picture sizes [640x480, 320x240], focus fixed, zoom false, flash null",
      "${cm}legacy preview at 320x240: first frame 115200 bytes, Y(10,100)=", 'previewEnabled true',
      "${cm}legacy preview frames keep coming: true", "${cm}legacy autoFocus true",
      "${cm}legacy takePicture: shutter, raw null, JPEG starts FFD8 true, decodes to 320x240; previewEnabled after false",
      "${cm}legacy released")) $Code "legacy $Mode"
}

# Geocoder: Windows.Services.Maps (MapLocationFinder) without a map service key. The app's
# Geocoder stays absent (isPresent false, IOException) while this is what Windows answers.
$geoScript = Join-Path $PSScriptRoot 'geocoder-provider-probe.ps1'
# Bound activation/COM startup as well as the asynchronous wait. Reading Result
# after a false Wait used to block until the entire test job timed out.
$geo = (DevRun 'powershell.exe' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $geoScript, (Join-Path $PSScriptRoot 'geocoder-operation.ps1')) 45).Trim()
if ($LASTEXITCODE -eq 0 -and $geo -match 'status: ([A-Za-z]+)') {
  Write-Host "PASS  devices: geocoder: Windows.Services.Maps without a map service key answers $($Matches[1]) (Geocoder stays absent: isPresent false, IOException)"
} else {
  Write-Host "FAIL  devices: geocoder: Windows.Services.Maps did not answer: $geo"
  $script:failed++
}

# Single process: the services in the app's own process (gesher-app --single-process).
$out = DevRun "$Dir/dalvikvm.exe" ($dvmDev + 'com.gesher.app.Main', (NoDrive "$Dir/devices.apk")) 400
$code = $LASTEXITCODE
LocationChecks 'single process' $out $code $false
SensorsEmptyCheck 'single process' $out $code
CameraEmptyCheck 'single process' $out $code
$env:GESHER_SENSOR_TEST = '1'
$env:GESHER_CAMERA_TEST = '1'
$env:GESHER_DEVICES_STEPS = 'sensors,camera'
$out = DevRun "$Dir/dalvikvm.exe" ($dvmDev + 'com.gesher.app.Main', (NoDrive "$Dir/devices.apk")) 400
$code = $LASTEXITCODE
SensorsTestCheck 'single process' $out $code
CameraTestCheck 'single process' $out $code
LegacyCameraCheck 'single process' $out $code
Remove-Item Env:GESHER_DEVICES_STEPS
# An app that asks for neither permission (tests/apps/nodevices): refused, as on Android.
$out = DevRun "$Dir/dalvikvm.exe" ($dvmDev + 'com.gesher.app.Main', (NoDrive "$Dir/nodevices.apk")) 400
$nd = 'nodevices: '
DevCheck 'devices: permissions (single process): without ACCESS_*_LOCATION LocationManager throws SecurityException; without CAMERA openCamera throws SecurityException' $out @(
  ,@("${nd}location: getLastKnownLocation throws SecurityException",
    "${nd}location: requestLocationUpdates throws SecurityException", "${nd}camera: 1 cameras",
    "${nd}camera: openCamera throws SecurityException", "${nd}done")) $LASTEXITCODE 'nodevices'

# The Gesher device: the location service in the system server, the app in a process
# of its own, across binder (listeners, the PendingIntent's broadcast, parcelled Locations).
# A device of its own, started with GESHER_SENSOR_TEST and GESHER_CAMERA_TEST (its app
# processes inherit them).
$launcherDev = $dvmDev + 'com.gesher.app.Launcher'
$null = DevRun "$Dir/dalvikvm.exe" ($launcherDev + 'shutdown') 120
$out = DevRun "$Dir/dalvikvm.exe" ($launcherDev + 'launch', (NoDrive "$Dir/devices.apk"), '--keep-task') 500
$code = $LASTEXITCODE
$serverLog = Join-Path $root 'logs/system_server.log'
$out += "`n" + (Get-Content $serverLog -Raw -ErrorAction SilentlyContinue)
$out += "`n" + (Get-Content (Join-Path $root 'logs/com.gesher.devices.log') -Raw -ErrorAction SilentlyContinue)
LocationChecks 'Gesher device' $out $code $true
SensorsTestCheck 'Gesher device' $out $code
CameraTestCheck 'Gesher device' $out $code
LegacyCameraCheck 'Gesher device' $out $code
# ACTION_IMAGE_CAPTURE: the device installed Gesher Camera (the PC has a camera: the test
# one); devices.apk's CaptureTestActivity asks it for a thumbnail and for a photo written
# to its provider's EXTRA_OUTPUT (Gesher Camera's auto-capture extra takes them).
$capOut = DevRun "$Dir/dalvikvm.exe" ($launcherDev + 'launch', (NoDrive "$Dir/devices.apk"), '--activity', '.CaptureTestActivity', '--keep-task') 400
$capCode = $LASTEXITCODE
$capOut += "`n" + (Get-Content $serverLog -Raw -ErrorAction SilentlyContinue)
$capOut += "`n" + (Get-Content (Join-Path $root 'logs/com.gesher.devices.log') -Raw -ErrorAction SilentlyContinue)
$capOut += "`n" + (Get-Content (Join-Path $root 'logs/com.gesher.camera.log') -Raw -ErrorAction SilentlyContinue)
# (Its thumbnail Bitmap reaches the caller across processes: Bitmaps parcel since the
# surfaces track, ADR 0056; 160 pixels wide, as Gesher Camera scales it.)
$thumb = if ($capOut -match 'devices: capture: thumbnail result OK, data (\S+)') { $Matches[1] } else { '?' }
DevCheck "devices: camera (Gesher device): ACTION_IMAGE_CAPTURE resolves to Gesher Camera (installed by the device as the PC has a camera); without EXTRA_OUTPUT it answers RESULT_OK (thumbnail: $thumb), with it it writes a 640x480 JPEG to the caller's provider's content URI" $capOut @(
  @('camera app: com.gesher.camera '),
  @('devices: capture: ACTION_IMAGE_CAPTURE resolves to com.gesher.camera/.CaptureActivity',
    'devices: capture: thumbnail result OK, data 160x120',
    'devices: capture: EXTRA_OUTPUT result OK, the file: ', 'starts FFD8 true, decodes to 640x480',
    'devices: capture: done')) $capCode 'capture'
if (-not $capOut.Contains('devices: capture: done')) {
  Write-Host '--- com.gesher.camera.log'
  Get-Content (Join-Path $root 'logs/com.gesher.camera.log') -ErrorAction SilentlyContinue | Select-Object -Last 80 | Out-Host
}
Remove-Item Env:GESHER_SENSOR_TEST
Remove-Item Env:GESHER_CAMERA_TEST
$null = DevRun "$Dir/dalvikvm.exe" ($launcherDev + 'shutdown') 120

