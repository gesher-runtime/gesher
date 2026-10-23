# Runs Android apps on the user's Gesher device: binderd, servicemanager and a
# system server that every launch shares, installed packages, and app
# processes the system server starts (docs/services.md, ADR 0026).
#
#   gesher-app my.apk                  install or update my.apk, start its
#                                      launcher activity (headless: once it is
#                                      idle the launcher closes its task) and
#                                      show the app's output until its process ends
#   gesher-app my.apk --keep-task      ... and let the app run until it finishes
#   gesher-app my.apk --activity .Other [--ez KEY true|false]...
#   gesher-app install my.apk          install or update the package
#   gesher-app uninstall com.example   uninstall a package
#   gesher-app list                    the installed packages
#   gesher-app status                  the device's processes
#   gesher-app shutdown                stop the device
#   gesher-app my.apk --single-process the app alone in this process, with
#                                      stand-in services (no device)
#   gesher-app my.apk --system-server  a device of this run only, the app in
#                                      this process attached to it
#
# The first launch starts the device detached (no console window: it outlives
# this console); later launches use it. It stops with `gesher-app shutdown`,
# or after GESHER_IDLE_TIMEOUT seconds (default 600) without an app process.
# Installed packages are in %LOCALAPPDATA%\Gesher\data\app, the logs in
# %LOCALAPPDATA%\Gesher\logs (<package>.log has an app's output).
#
# gesher-app.cmd runs this script. Like gesher.ps1, APKs must be on the same
# drive as Gesher (ART's class paths are ':'-separated, so paths go without the
# drive letter) and the runtime's directories are in %LOCALAPPDATA%\Gesher.
param(
  [Parameter(ValueFromRemainingArguments)] [string[]] $Arguments = @()
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = Join-Path $env:LOCALAPPDATA 'Gesher'
$commands = 'install', 'uninstall', 'list', 'status', 'start', 'shutdown'
if ($Arguments.Count -eq 0) {
  Write-Host 'usage: gesher-app APK [--keep-task] [--activity CLASS] [--ez KEY true|false]... [--single-process | --system-server]'
  Write-Host '       gesher-app install APK | uninstall PACKAGE | list | status | start | shutdown'
  exit 2
}
$command = $Arguments[0]
$rest = @($Arguments | Select-Object -Skip 1)

# Reads the device's state file (Java properties: key=value, '\' escapes).
function DeviceState {
  $file = Join-Path $root 'data\system\gesher-device'
  if (-not (Test-Path $file)) { return $null }
  $state = @{}
  foreach ($line in Get-Content $file) {
    if ($line -match '^\s*[#!]' -or $line -notmatch '=') { continue }
    $k, $v = $line -split '=', 2
    $state[$k.Trim()] = ($v -replace '\\(.)', '$1')
  }
  return $state
}

# Whether the device's process `name` still runs (the same process: pid and start time).
function DeviceProcess($state, [string] $name) {
  $id = [int]$state[$name]
  if ($id -le 0) { return $null }
  $p = Get-Process -Id $id -ErrorAction SilentlyContinue
  if (-not $p) { return $null }
  try {
    if ($state["$name.start"] -and $p.StartTime.ToFileTimeUtc() -ne [long]$state["$name.start"]) { return $null }
  } catch { }
  return $p
}

# `gesher-app shutdown`, without starting a runtime: the system server's
# shutdown event, then whatever is left is ended.
if ($command -eq 'shutdown') {
  $state = DeviceState
  if (-not $state) { Write-Host 'gesher-app: no Gesher device is running'; exit 0 }
  $server = DeviceProcess $state 'system_server'
  if ($server) {
    try {
      $event = [Threading.EventWaitHandle]::OpenExisting($state['shutdown_event'])
      $null = $event.Set()
      $event.Dispose()
    } catch { }
    $null = $server.WaitForExit(30000)
  }
  foreach ($name in 'system_server', 'servicemanager', 'binderd') {
    $p = DeviceProcess $state $name
    if ($p) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  }
  Remove-Item (Join-Path $root 'data\system\gesher-device'), (Join-Path $root 'data\system\gesher-device.ready') -ErrorAction SilentlyContinue
  Write-Host "gesher-app: the Gesher device $($state['device']) stopped"
  exit 0
}

if ($root.Substring(0, 2) -ne $here.Substring(0, 2)) {
  throw "$root must be on drive $($here.Substring(0, 2)), with Gesher."
}
$apkFull = $null
if ($commands -notcontains $command) {
  $apkFull = (Resolve-Path $command).Path
} elseif ($command -eq 'install' -and $rest.Count -gt 0) {
  $apkFull = (Resolve-Path $rest[0]).Path
  $rest = @($rest | Select-Object -Skip 1)
}
if ($apkFull -and $apkFull.Substring(0, 2) -ne $here.Substring(0, 2)) {
  throw "$apkFull must be on drive $($here.Substring(0, 2)), with Gesher."
}
Set-Location $here
function NoDrive([string] $p) { ($p -replace '\\', '/') -replace '^[A-Za-z]:', '' }

foreach ($d in 'system/framework', 'art', 'data', 'i18n/etc/icu', 'tzdata') {
  New-Item -ItemType Directory -Force (Join-Path $root $d) | Out-Null
}
# ICU's data, the framework's resources and the system properties, where Android
# keeps them; refreshed when a new version of Gesher brings different ones.
function Install([string] $from, [string] $to) {
  if (-not (Test-Path $to) -or (Get-Item $to).Length -ne (Get-Item $from).Length) {
    Copy-Item $from $to
  }
}
$data = Get-Item (Join-Path $here 'icudt*l.dat')
Install $data.FullName (Join-Path $root "i18n/etc/icu/$($data.Name)")
Install (Join-Path $here 'framework-res.apk') (Join-Path $root 'system/framework/framework-res.apk')
Install (Join-Path $here 'build.prop') (Join-Path $root 'system/build.prop')
# The system fonts and fonts.xml (docs/fonts.md), as /system/fonts and /system/etc.
if (Test-Path (Join-Path $here 'fonts/fonts.xml')) {
  foreach ($d in 'system/fonts', 'system/etc') { New-Item -ItemType Directory -Force (Join-Path $root $d) | Out-Null }
  Install (Join-Path $here 'fonts/fonts.xml') (Join-Path $root 'system/etc/fonts.xml')
  foreach ($f in Get-ChildItem (Join-Path $here 'fonts') -Filter *.ttf) {
    Install $f.FullName (Join-Path $root "system/fonts/$($f.Name)")
  }
}

$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"

# gesher-app.jar comes after framework.jar: its placeholders for classes
# framework.jar lacks must not shadow real ones.
$jars = 'core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app'
$bcp = ($jars | ForEach-Object { NoDrive "$here/$_.jar" }) -join ':'
$dvm = @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system/framework/boot.art")", '-Xint', '-Xusejit:false')
$dalvikvm = Join-Path $here 'dalvikvm.exe'

# The device's commands, and launching through it (com.gesher.app.Launcher).
if ($commands -contains $command) {
  $launcherArgs = @($command)
  if ($apkFull) { $launcherArgs += NoDrive $apkFull }
  & $dalvikvm @dvm com.gesher.app.Launcher @launcherArgs @rest
  exit $LASTEXITCODE
}
if ($rest -notcontains '--single-process' -and $rest -notcontains '--system-server') {
  & $dalvikvm @dvm com.gesher.app.Launcher launch (NoDrive $apkFull) @rest
  exit $LASTEXITCODE
}

# Single process: the app alone, with stand-in services in its own process.
$DriverArgs = @($rest | Where-Object { $_ -ne '--single-process' })
if ($DriverArgs -notcontains '--system-server') {
  & $dalvikvm @dvm com.gesher.app.Main @DriverArgs (NoDrive $apkFull)
  exit $LASTEXITCODE
}

# A device of this run only: binderd, servicemanager and the system server with
# the APK, then the app in this console's process; the three are stopped when it exits.
$logs = Join-Path $root 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$env:GESHER_BINDER = "gesher-app-$PID"
$started = @()
try {
  $started += Start-Process (Join-Path $here 'binderd.exe') -ArgumentList $env:GESHER_BINDER -PassThru -NoNewWindow `
    -RedirectStandardOutput "$logs/run-binderd.out" -RedirectStandardError "$logs/run-binderd.log"
  $started += Start-Process (Join-Path $here 'servicemanager.exe') -PassThru -NoNewWindow `
    -RedirectStandardOutput "$logs/run-servicemanager.out" -RedirectStandardError "$logs/run-servicemanager.log"
  $started += Start-Process $dalvikvm -PassThru -NoNewWindow `
    -ArgumentList ($dvm + 'com.gesher.server.SystemServer', (NoDrive $apkFull)) `
    -RedirectStandardOutput "$logs/run-system_server.out" -RedirectStandardError "$logs/run-system_server.log"
  & $dalvikvm @dvm com.gesher.app.Main @DriverArgs (NoDrive $apkFull)
  $code = $LASTEXITCODE
  if ($code -ne 0) { Write-Host "gesher-app: the system server's logs are in $logs" }
} finally {
  foreach ($p in $started) {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  }
  Remove-Item Env:GESHER_BINDER
}
exit $code
