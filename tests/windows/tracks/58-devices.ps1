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
  $p = [Diagnostics.Process]::Start($psi)
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEndAsync()
  $timedOut = -not $p.WaitForExit($Seconds * 1000)
  if ($timedOut) {
    $p.Kill($true)
    $p.WaitForExit()
  }
  $null = $stdout.Wait(10000), $stderr.Wait(10000)
  $global:LASTEXITCODE = if ($timedOut) { -1 } else { $p.ExitCode }
  $out = $stdout.Result + $stderr.Result
  if ($timedOut) { $out += "`n(timed out after $Seconds s)" }
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
    ($Out -split "`n" | Where-Object { $_ -match 'devices|location|gesher-app|system_server|Exception|Error|FATAL| at ' } |
      Select-Object -First 200) | Out-Host
  }
}

Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework') -Force
Copy-Item "$Dir/build.prop" (Join-Path $root 'system') -Force
$bcpDev = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmDev = @("-Xbootclasspath:$bcpDev", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
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
    @('location: registerLocationPendingIntent gps for com.gesher.devices', $pi)) $Code $Mode
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

# Single process: the services in the app's own process (gesher-app --single-process).
$out = DevRun "$Dir/dalvikvm.exe" ($dvmDev + 'com.gesher.app.Main', (NoDrive "$Dir/devices.apk")) 400
LocationChecks 'single process' $out $LASTEXITCODE $false

# The Gesher device: the location service in the system server, the app in a process
# of its own, across binder (listeners, the PendingIntent's broadcast, parcelled Locations).
$launcherDev = $dvmDev + 'com.gesher.app.Launcher'
$out = DevRun "$Dir/dalvikvm.exe" ($launcherDev + 'launch', (NoDrive "$Dir/devices.apk"), '--keep-task') 500
$code = $LASTEXITCODE
$serverLog = Join-Path $root 'logs/system_server.log'
$out += "`n" + (Get-Content $serverLog -Raw -ErrorAction SilentlyContinue)
LocationChecks 'Gesher device' $out $code $true
$null = DevRun "$Dir/dalvikvm.exe" ($launcherDev + 'shutdown') 120
