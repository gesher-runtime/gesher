# CI-only exact current Java/native component: real Web3 query/lifetime correction.
param([Parameter(Mandatory)][string] $Dir)
$ErrorActionPreference = 'Stop'
$failed = 0
$Dir = (Resolve-Path -LiteralPath $Dir).Path
function NoDrive([string] $p) { ($p -replace '\\', '/') -replace '^[A-Za-z]:', '' }
$root = Join-Path $env:RUNNER_TEMP "web3-query-component-$env:GITHUB_RUN_ID"
if (Test-Path -LiteralPath $root) { throw 'Refusing to reuse an existing probe root' }
foreach ($directory in 'system/framework/x86_64','system/fonts','system/etc','art','data','i18n/etc/icu','tzdata') {
  New-Item -ItemType Directory -Force (Join-Path $root $directory) | Out-Null
}
Copy-Item "$Dir/icudt*l.dat" (Join-Path $root 'i18n/etc/icu')
if (Test-Path "$Dir/tzdata") { Copy-Item -Recurse -Force "$Dir/tzdata/*" (Join-Path $root 'tzdata') }
Copy-Item "$Dir/framework/x86_64/*" (Join-Path $root 'system/framework/x86_64')
Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework')
Copy-Item "$Dir/build.prop" (Join-Path $root 'system')
Copy-Item "$Dir/fonts/*.ttf" (Join-Path $root 'system/fonts')
Copy-Item "$Dir/fonts/fonts.xml" (Join-Path $root 'system/etc/fonts.xml')
Copy-Item "$Dir/fonts/windows-fonts.xml" (Join-Path $root 'system/etc/windows-fonts.xml')
$fontLink = Join-Path $root 'windows-fonts'
New-Item -ItemType Junction -Path $fontLink -Target (Join-Path $env:WINDIR 'Fonts') | Out-Null
$env:GESHER_WINDOWS_FONTS = NoDrive $fontLink
$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"
$env:GESHER_REQUIRE_BOOT_IMAGE = '1'
$bcp = (('core-oj','core-libart','core-icu4j','framework','gesher-app') |
  ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$vm = @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system/framework/boot.art")", '-Xint', '-Xusejit:false')
# A separate negative control explicitly executes the unwrapped real JNI on the
# owner STA. Its expected external timeout is recorded, never called an app pass.
function InvokeBoundaryFixture([bool] $Unwrapped) {
  $label = if ($Unwrapped) { 'unwrapped-control' } else { 'bounded-production' }
  $stdout = Join-Path $root "$label.stdout.log"
  $stderr = Join-Path $root "$label.stderr.log"
  $arguments = $vm + @('-cp', (NoDrive "$Dir/web3-query-tests.dex"),
    'com.gesher.app.accessibility.QueryBoundaryTest', (NoDrive "$Dir/build.prop"),
    (NoDrive "$Dir/framework-res.apk"), $label)
  $quoted = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
  $process = $null
  try {
    $process = Start-Process -FilePath "$Dir/dalvikvm.exe" -ArgumentList $quoted -WindowStyle Hidden -PassThru `
      -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $exited = $process.WaitForExit($(if ($Unwrapped) { 15000 } else { 30000 }))
    $out = Get-Content -LiteralPath $stdout -Raw -ErrorAction SilentlyContinue
    if ($Unwrapped) {
      if ($exited -or -not $out.Contains('ACTUAL_GLOBAL_MONITOR_WAITER') -or
          -not $out.Contains('ACTUAL_CLOSE_UNDER_GLOBAL_LOCK kind=0 direct=true')) {
        throw 'Unwrapped real JNI control did not demonstrate the actual global-lock/owner-join deadlock'
      }
      Write-Host 'EXPECTED_UNWRAPPED_FAILURE real held global mLock + owner JNI + actual window join remains stuck after15s; terminating only owned control'
    } else {
      if (-not $exited) { throw 'Actual query-bound fixture did not exit within external30s bound' }
      $process.Refresh()
      if ($process.ExitCode -ne 0 -or -not $out.Contains('ACTUAL_QUERY_BOUNDARY_SUMMARY four-kinds=4 owner-join=4 detached=0 cache=0')) {
        throw "Actual query-bound fixture exit=$($process.ExitCode); original real-lock/VM-drain controls incomplete"
      }
      Write-Host 'PASS web3 query: actual4wrappers/held-global-lock/owner-join/repeated-window/inflight-shutdown/detach0/cache0/freshprocess0'
    }
  } finally {
    if ($null -ne $process) {
      if (-not $process.HasExited) {
        $process.Kill($true)
        if (-not $process.WaitForExit(5000)) { throw 'Owned boundary fixture failed finite termination after kill' }
      }
      $process.Dispose()
    }
    if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout | Out-Host }
    if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr | Out-Host }
  }
}
InvokeBoundaryFixture $true
InvokeBoundaryFixture $false

# Execute the original14 real Web3 cases, with their original400/60+60s,
# expected output and actual fresh app-process exit-zero checks unchanged.
. "$PSScriptRoot/tracks/54-web3.ps1"
if ($null -eq $sd) { throw 'Original Web3 device shutdown process was not recorded' }
$sd.Refresh()
if (-not $sd.HasExited -or $sd.ExitCode -ne 0) {
  Write-Host 'FAIL web3 query: original device shutdown did not actually finish0'
  $failed++
} else { Write-Host 'PASS web3 query: original device shutdown actually exits0' }
if ($sd.HasExited) { $sd.Dispose() }
$originalFailed = $failed
# Reuse the real harness/strict child exit checks for two mapped Dialog cycles
# through Main and through a real device app process; retain all originals.
$mappedExpect = @('Web3 query boundary mapped done: positive=2 canceled=2 stale=2 attached=0')
Web3ScreenReader $true
try {
  $null = Web3Run 'real mapped Dialog: actual positive/canceled/stale actions twice (single process)' `
    @('com.gesher.app.Main','--window','--activity','com.gesher.web3app.QueryBoundaryActivity', (NoDrive "$Dir/web3app.apk")) `
    ($mappedExpect + 'gesher-app: done') 'Web3 query boundary mapped done'
  $env:ANDROID_DATA = NoDrive "$root/data-query-device"
  $env:GESHER_LOGS = NoDrive "$root/logs-query-device"
  New-Item -ItemType Directory -Force "$root/data-query-device" | Out-Null
  $env:GESHER_SHORTCUTS = '0'
  $null = Web3Run 'real mapped Dialog: actual positive/canceled/stale actions twice (device app process)' `
    @('com.gesher.app.Launcher','launch',(NoDrive "$Dir/web3app.apk"),'--window','--activity','com.gesher.web3app.QueryBoundaryActivity') `
    $mappedExpect 'Web3 query boundary mapped done'
} finally {
  Web3ScreenReader $false
  # Fresh real device shutdown must finish0, never just print a done marker.
  $stdout = Join-Path $root 'query-device-shutdown.stdout.log'
  $stderr = Join-Path $root 'query-device-shutdown.stderr.log'
  $quoted = (($vm + @('com.gesher.app.Launcher','shutdown')) | ForEach-Object { '"' + $_ + '"' }) -join ' '
  $shutdown = $null
  try {
    $shutdown = Start-Process -FilePath "$Dir/dalvikvm.exe" -ArgumentList $quoted -WindowStyle Hidden -PassThru `
      -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if (-not $shutdown.WaitForExit(120000)) { throw 'Actual fresh device shutdown exceeded original120s' }
    $shutdown.Refresh()
    if ($shutdown.ExitCode -ne 0) { throw "Actual fresh device shutdown exit=$($shutdown.ExitCode)" }
    Write-Host 'PASS web3 query: fresh actual device shutdown process exits0'
  } finally {
    if ($null -ne $shutdown) {
      if (-not $shutdown.HasExited) {
        $shutdown.Kill($true)
        if (-not $shutdown.WaitForExit(5000)) { throw 'Owned shutdown process failed finite termination' }
      }
      $shutdown.Dispose()
    }
    if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout | Out-Host }
    if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr | Out-Host }
  }
}
Write-Host "ACTUAL_WEB3_QUERY_COMPONENT source=$env:GITHUB_SHA original-failures=$originalFailed total-failures=$failed; component-only"
if ($failed) { exit 1 }
