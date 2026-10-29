# CI-only actual installed APK route under one explicit, receipted producer role.
param([Parameter(Mandatory)][string] $Dir,
      [Parameter(Mandatory)][string] $RuntimeExe,
      [Parameter(Mandatory)][ValidateSet('production','query-hook')][string] $Role)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'CI only' }
$Dir = (Resolve-Path -LiteralPath $Dir).Path
$RuntimeExe = (Resolve-Path -LiteralPath $RuntimeExe).Path
$expected = if ($Role -ceq 'query-hook') { 'dalvikvm-web3-query-test.exe' } else { 'dalvikvm.exe' }
if ((Split-Path -Leaf $RuntimeExe) -cne $expected -or (Split-Path -Parent $RuntimeExe) -cne $Dir) {
  throw 'explicit runtime/producer role mismatch'
}
if ($Role -ceq 'query-hook') { python tools/web3/host_roles.py verify --dist $Dir --hook }
else { python tools/web3/host_roles.py verify --dist $Dir }
if ($LASTEXITCODE -ne 0) { throw 'current source/compiler/role receipt failed' }
. "$PSScriptRoot/helpers/CloseProcessReaders.ps1"
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile("$PSScriptRoot/tracks/50-services.ps1", [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw $parseErrors[0] }
$definitions = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false) | Where-Object Name -eq 'RunTimed')
if ($definitions.Count -ne 1) { throw 'qualified exact-process supervisor missing' }
. ([scriptblock]::Create($definitions[0].Extent.Text))
function NoDrive([string] $path) { ($path -replace '\\','/') -replace '^[A-Za-z]:','' }
$root = Join-Path $env:RUNNER_TEMP "binder-installed-$Role-$env:GITHUB_RUN_ID-$env:GITHUB_RUN_ATTEMPT"
if (Test-Path -LiteralPath $root) { throw 'refusing an existing installed-guest root' }
foreach ($directory in 'system/framework/x86_64','system/fonts','system/etc','art','data','i18n/etc/icu','tzdata','logs') {
  New-Item -ItemType Directory (Join-Path $root $directory) | Out-Null
}
Copy-Item "$Dir/icudt*l.dat" (Join-Path $root 'i18n/etc/icu')
Copy-Item "$Dir/framework/x86_64/*" (Join-Path $root 'system/framework/x86_64')
Copy-Item "$Dir/framework-res.apk" (Join-Path $root 'system/framework')
Copy-Item "$Dir/build.prop" (Join-Path $root 'system')
Copy-Item "$Dir/fonts/*.ttf" (Join-Path $root 'system/fonts')
Copy-Item "$Dir/fonts/fonts.xml" (Join-Path $root 'system/etc/fonts.xml')
Copy-Item "$Dir/fonts/windows-fonts.xml" (Join-Path $root 'system/etc/windows-fonts.xml')
Copy-Item "$Dir/tzdata/*" (Join-Path $root 'tzdata') -Recurse
$env:ANDROID_ROOT = NoDrive "$root/system"
$env:ANDROID_ART_ROOT = NoDrive "$root/art"
$env:ANDROID_DATA = NoDrive "$root/data"
$env:ANDROID_I18N_ROOT = NoDrive "$root/i18n"
$env:ANDROID_TZDATA_ROOT = NoDrive "$root/tzdata"
$env:GESHER_LOGS = NoDrive "$root/logs"
$env:GESHER_REQUIRE_BOOT_IMAGE = '1'
$bcp = (('core-oj','core-libart','core-icu4j','framework','gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$launcher = @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system/framework/boot.art")", '-Xint', '-Xusejit:false', 'com.gesher.app.Launcher')
Write-Host "ACTUAL_INSTALLED_ROLE role=$Role executable=$RuntimeExe sha256=$((Get-FileHash -LiteralPath $RuntimeExe -Algorithm SHA256).Hash) source=$env:GITHUB_SHA run=$env:GITHUB_RUN_ID attempt=$env:GITHUB_RUN_ATTEMPT"
$problem = $null
$ownedServers = @()
$state = @{}
function RetainOwnedDevice {
  $stateFile = Join-Path $root 'data/system/gesher-device'
  if (-not (Test-Path -LiteralPath $stateFile)) { return }
  foreach ($line in Get-Content -LiteralPath $stateFile) {
    if ($line -match '^\s*[#!]' -or $line -notmatch '=') { continue }
    $key, $value = $line -split '=',2
    $script:state[$key.Trim()] = $value -replace '\\(.)','$1'
  }
  foreach ($name in 'binderd','servicemanager','system_server') {
    if (@($script:ownedServers | Where-Object Name -ceq $name).Count) { continue }
    $id = [int]$state[$name]; $birth = [int64]$state["$name.start"]
    if ($id -le 0 -or $birth -le 0) { throw 'missing exact device instance identity' }
    $process = [Diagnostics.Process]::GetProcessById($id)
    try {
      $handle = $process.SafeHandle # retain BEFORE querying birth or module
      if ($process.StartTime.ToFileTimeUtc() -ne $birth) { throw 'device birth differs from bootstrap record' }
      $expectedModule = if ($name -ceq 'system_server') { $RuntimeExe } else { Join-Path $Dir "$name.exe" }
      if ($process.MainModule.FileName -ine $expectedModule) { throw 'device module differs from current explicit producer' }
      $script:ownedServers += @{Name=$name; Process=$process; Handle=$handle; Birth=$birth}
      Write-Host "ACTUAL_INSTALLED_OWNER role=$Role name=$name pid=$id birth=$birth module=$expectedModule retained=1"
    } catch { $process.Dispose(); throw }
  }
}
try {
  $output = RunTimed $RuntimeExe ($launcher + @('install', (NoDrive "$Dir/lifecycle.apk"))) 180
  $code = $LASTEXITCODE; Write-Host $output
  RetainOwnedDevice
  if ($code -ne 0 -or -not $output.Contains('installed com.gesher.lifecycle')) { throw 'actual installed package setup failed' }
  $seenInstances = @{}; $seenEndpoints = @{}; $expectedSid = $null
  for ($launchIndex = 0; $launchIndex -lt 3; $launchIndex++) {
  $output = RunTimed $RuntimeExe ($launcher + @('launch','com.gesher.lifecycle','--headless')) 180
  $code = $LASTEXITCODE; Write-Host $output
  if ($code -ne 0) { throw "actual installed child failed: $code" }
  foreach ($row in @('installed restricted guest: actual token zero-capabilities, data read/write, code/config write and host endpoint denied',
      'attached to the system server','LifecycleApplication onCreate, package=com.gesher.lifecycle',
      'greeting=Hello from a resource','startActivityAndWait: result 0, com.gesher.lifecycle/.LifecycleActivity',
      'the process of com.gesher.lifecycle ended (exit code 0)')) {
    if (-not $output.Contains($row)) { throw "missing actual installed APK observation: $row" }
  }
  $server = Get-Content -LiteralPath (Join-Path $root 'logs/system_server.log') -Raw
  Write-Host $server
  $observations = [regex]::Matches($server, 'INSTALLED_GUEST suspended pid=(\d+) birth=(\d+) uid=(\d+) sid=(S-1-15-2-[0-9-]+) capabilities=0 ownedjob=1 endpoint=([a-f0-9]{32})')
  if ($observations.Count -ne $launchIndex + 1) { throw 'unexpected number of actual restricted child instances' }
  $actual = $observations[$observations.Count - 1]
  $child = [regex]::Match($output, 'app process for com\.gesher\.lifecycle: pid (\d+)')
  if (-not $actual.Success -or -not $child.Success -or $actual.Groups[1].Value -cne $child.Groups[1].Value -or
      [uint64]$actual.Groups[2].Value -eq 0 -or [int]$actual.Groups[3].Value -lt 10000) {
    throw 'actual suspended owner/token did not match the APK process'
  }
  $instance = $actual.Groups[1].Value + ':' + $actual.Groups[2].Value
  $endpoint = $actual.Groups[5].Value
  if ($seenInstances.ContainsKey($instance) -or $seenEndpoints.ContainsKey($endpoint)) { throw 'relaunch reused an instance or endpoint namespace' }
  $seenInstances[$instance] = $true; $seenEndpoints[$endpoint] = $true
  if ($expectedSid -and $expectedSid -cne $actual.Groups[4].Value) { throw 'same install changed its profile identity during relaunch' }
  $expectedSid = $actual.Groups[4].Value
  Write-Host "ACTUAL_INSTALLED_GUEST role=$Role launch=$launchIndex child=$($child.Groups[1].Value) birth=$($actual.Groups[2].Value) token_sid=$($actual.Groups[4].Value) endpoint=$endpoint apk_boot=1 binder_attach=1 exit=0"
  }
  Write-Host "ACTUAL_INSTALLED_RELAUNCH role=$Role launches=3 unique_instances=3 unique_endpoint_names=3 same_install_sid=1 intentional_delay=0"
} catch { $problem = $_ }
finally {
  # No numeric-PID cleanup: retain and validate each actual bootstrap process,
  # signal only this owned root's event, then wait/kill through retained handles.
  try {
    if ($ownedServers.Count -ne 3) { RetainOwnedDevice }
    if ($ownedServers.Count -ne 3) { throw 'device ownership unavailable for confirmed cleanup' }
    $eventName = [string]$state['shutdown_event']
    if ($eventName -cnotmatch '^Local\\gesher-[a-f0-9]+-shutdown$') { throw 'unexpected owned shutdown event name' }
    $event = [Threading.EventWaitHandle]::OpenExisting($eventName)
    try { if (-not $event.Set()) { throw 'owned shutdown event was not signaled' } }
    finally { $event.Dispose() }
    foreach ($owner in ($ownedServers | Sort-Object @{Expression={ if ($_.Name -ceq 'system_server') { 0 } else { 1 } }})) {
      if (-not $owner.Process.WaitForExit(15000)) {
        $problem = "owned $($owner.Name) did not retire; prior=$problem"
        $owner.Process.Kill() # Process caches the retained SafeHandle above
        if (-not $owner.Process.WaitForExit(5000)) { throw 'owned process retirement remains unconfirmed' }
      }
      if ($owner.Name -ceq 'system_server' -and $owner.Process.ExitCode -ne 0) {
        $problem = "installed policy shutdown exit=$($owner.Process.ExitCode); prior=$problem"
      }
      Write-Host "ACTUAL_INSTALLED_RETIRE role=$Role name=$($owner.Name) birth=$($owner.Birth) exit=$($owner.Process.ExitCode) signaled=1"
    }
  } catch { $problem = "owned installed device cleanup failed: $_; prior=$problem" }
  finally {
    foreach ($owner in $ownedServers) {
      $owner.Process.Dispose()
      if (-not $owner.Handle.IsClosed) { $problem = "retained device handle did not close; prior=$problem" }
    }
  }
}
if ($problem) { throw $problem }
Write-Host "PASS actual installed restricted guest role=$Role source=$env:GITHUB_SHA"
