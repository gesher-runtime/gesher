# Runs an Android app's launcher Activity headless with gesher-app: the app is
# bound, its Activity attached, created, started and resumed on a main Looper,
# and the process exits (see app/README.md).
#
#   gesher-app my.apk
#   gesher-app my.apk --activity .OtherActivity
#   gesher-app my.apk --system-server
#
# --system-server runs the system services in a process of their own, as on
# Android: binderd (the binder driver) and servicemanager for a binder device
# of this run, then com.gesher.server.SystemServer with the APK, then the app
# in another process, which gets the package and every service through
# servicemanager and attaches to the server, which drives its activity
# (docs/decisions/0024-services-app-process.md); the three are stopped when
# the app exits.
#
# gesher-app.cmd runs this script. Like gesher.ps1, the APK must be on the same
# drive as Gesher (ART's class paths are ':'-separated, so paths go without the
# drive letter) and the runtime's directories are in %LOCALAPPDATA%\Gesher.
param(
  [Parameter(Mandatory)] [string] $Apk,
  [Parameter(ValueFromRemainingArguments)] [string[]] $DriverArgs = @()
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

$apkFull = (Resolve-Path $Apk).Path
$root = Join-Path $env:LOCALAPPDATA 'Gesher'
foreach ($p in $apkFull, $root) {
  if ($p.Substring(0, 2) -ne $here.Substring(0, 2)) {
    throw "$p must be on drive $($here.Substring(0, 2)), with Gesher."
  }
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
if ($DriverArgs -notcontains '--system-server') {
  & $dalvikvm @dvm com.gesher.app.Main @DriverArgs (NoDrive $apkFull)
  exit $LASTEXITCODE
}

# Multi-process: binderd, servicemanager and the system server, then the app.
$logs = Join-Path $root 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$env:GESHER_BINDER = "gesher-app-$PID"
$started = @()
try {
  $started += Start-Process (Join-Path $here 'binderd.exe') -ArgumentList $env:GESHER_BINDER -PassThru -NoNewWindow `
    -RedirectStandardOutput "$logs/binderd.out" -RedirectStandardError "$logs/binderd.log"
  $started += Start-Process (Join-Path $here 'servicemanager.exe') -PassThru -NoNewWindow `
    -RedirectStandardOutput "$logs/servicemanager.out" -RedirectStandardError "$logs/servicemanager.log"
  $started += Start-Process $dalvikvm -PassThru -NoNewWindow `
    -ArgumentList ($dvm + 'com.gesher.server.SystemServer', (NoDrive $apkFull)) `
    -RedirectStandardOutput "$logs/system_server.out" -RedirectStandardError "$logs/system_server.log"
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
