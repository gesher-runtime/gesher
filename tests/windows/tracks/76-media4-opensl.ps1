# Real OpenSL identifiers/query/negotiation/permission contracts (media4, ADR0064).
# Includes the independent native launcher over the same functional backend.
$slHost = (Resolve-Path $Dir).Path
$slDir = Join-Path $slHost 'media/opensl-contracts'
$slExpected = @(
  'media4 opensl data: ok; 53 canonical UUID pointer objects, exact pinned fields and ELF indirection',
  'media4 opensl query: ok; typed global and engine queries, supported subsets, null/index/object controls',
  'media4 opensl negotiation: ok; copied UUIDs, all 53 required/optional engine outcomes, duplicate and invalid arrays',
  'media4 opensl player: ok; real PCM queue and volume, explicit exposure and unsupported effect rejection',
  'media4 opensl callback trace: calls 1, TLS valid true, caller TLS unchanged true',
  'media4 opensl volume trace: requested -600, set 0, get 0, value -600',
  'media4 opensl volume trace: requested -32768, set 0, get 0, value -32768',
  'media4 opensl volume trace: requested -1, set 0, get 0, value -1',
  'media4 opensl volume trace: requested 0, set 0, get 0, value 0',
  'media4 opensl: all passed')

. "$PSScriptRoot/../helpers/CloseProcessReaders.ps1"

function InvokeOpenSl([string[]] $Arguments, [hashtable] $Environment = @{}, [string] $Exe = "$slHost/dalvikvm.exe") {
  $info = [Diagnostics.ProcessStartInfo]::new($Exe)
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  foreach ($a in $Arguments) { $info.ArgumentList.Add($a) }
  $info.Environment['GESHER_AUDIO'] = 'null'
  $info.Environment['GESHER_SHORTCUTS'] = '0'
  $info.Environment['GESHER_MEDIA_TRACE_RECORD'] = '1'
  foreach ($key in $Environment.Keys) { $info.Environment[$key] = $Environment[$key] }
  $process = [Diagnostics.Process]::Start($info)
  $nativeHandle = $process.SafeHandle
  $cancellation = [Threading.CancellationTokenSource]::new()
  $pipes = @(@{ Name = 'stdout'; Reader = $null; Handle = $null; Raw = [IntPtr]::Zero; Task = $null; Copied = $false },
             @{ Name = 'stderr'; Reader = $null; Handle = $null; Raw = [IntPtr]::Zero; Task = $null; Copied = $false })
  $text = ''
  $timedOut = $true
  $nativeExit = $null
  try {
  foreach ($pipe in $pipes) {
      $pipe.Reader = if ($pipe.Name -eq 'stdout') { $process.StandardOutput } else { $process.StandardError }
      $pipe.Handle = $pipe.Reader.BaseStream.SafeFileHandle
      $pipe.Raw = $pipe.Handle.DangerousGetHandle()
      $pipe.Task = $pipe.Reader.ReadToEndAsync($cancellation.Token)
    }
  $timedOut = -not $process.WaitForExit(120000)
  $text = ''
  if ($timedOut) {
    $text += "`n(timed out after 120 s)"
    try { $process.Kill($true) } catch { $text += "`n(process kill failed: $_)" }
    if (-not $process.WaitForExit(5000)) { $text += "`n(process still alive five seconds after kill)" }
  }
  foreach ($pipe in $pipes) {
    try {
      if ($pipe.Task.Wait(5000)) { $text += $pipe.Task.Result; $pipe.Copied = $true }
      else { $timedOut = $true; $text += "`n($($pipe.Name) did not complete within five seconds)" }
    } catch { $timedOut = $true; $text += "`n($($pipe.Name) read failed: $_)" }
  }
  if ($process.HasExited) { $nativeExit = $process.ExitCode }
  } finally {
    $cleanup = @{ LateText = ''; TasksComplete = $false; PipesClosed = $false; Problem = 'reader cleanup did not complete' }
    try { $cleanup = Close-ProcessReaders $cancellation $pipes }
    catch { $timedOut = $true; $text += "`n(reader cleanup failed: $_)" }
    $text += $cleanup.LateText
    if (-not $cleanup.TasksComplete -or -not $cleanup.PipesClosed -or $cleanup.Problem) {
      $timedOut = $true
      $text += "`n(reader cleanup incomplete: $($cleanup.Problem))"
    }
    try {
      if (-not $process.HasExited) {
        $process.Kill($true)
        if (-not $process.WaitForExit(5000)) { $timedOut = $true; $text += "`n(final owned process cleanup exceeded five seconds)" }
      }
    } catch { $timedOut = $true; $text += "`n(final owned process cleanup failed: $_)" }
    try { $process.Dispose() } catch { $timedOut = $true; $text += "`n(process disposal failed: $_)" }
    if (-not $nativeHandle.IsClosed) { $timedOut = $true; $text += "`n(native process handle did not close)" }
    if ($env:GESHER_CI_PROCESS_TRACE -eq '1') {
      $text += "`nINFO READER_CLEANUP helper=InvokeOpenSl tasksdone=$($cleanup.TasksComplete) pipesclosed=$($cleanup.PipesClosed) processclosed=$($nativeHandle.IsClosed) pipe-output=$($pipes[0].Raw) pipe-error=$($pipes[1].Raw)"
    }
  }
  $code = if ($timedOut) { -1 } else { $nativeExit }
  return @{ Text = $text; Code = $code }
}

function CheckOpenSl([string] $Abi, [string] $Mode, [bool] $Allowed, [hashtable] $Result, [string[]] $Extra, [string] $Caller = 'JNI') {
  $permission = if ($Mode -eq 'core JNI') { 'bare core without recording identity returns 0x0B without object' }
  elseif ($Allowed) { 'manifest allowed native recorder lifecycle' } elseif ($Caller -eq 'executable') {
    'native process without recording identity returns 0x0B without object'
  } else {
    'manifest denied native recorder returns 0x0B without object'
  }
  $permissionTrace = if ($Allowed) { 'expected allowed, result 0, object true' } else {
    'expected denied, result 11, object false'
  }
  $missing = @(($slExpected + "media4 opensl ABI: $Abi" + "media4 opensl caller: $Caller" +
    "media4 opensl permission: ok; $permission" + "media4 opensl permission trace: $permissionTrace" + $Extra) |
    Where-Object { -not $Result.Text.Contains($_) })
  if ($Result.Code -eq 0 -and $missing.Count -eq 0 -and
      -not $Result.Text.Contains('media4 opensl failure:')) {
    Write-Host "PASS  media4 opensl ($Abi; $Mode): actual canonical data, typed queries, interface negotiation and recording permission"
  } else {
    Write-Host "FAIL  media4 opensl ($Abi; $Mode) (exit $($Result.Code); missing: $($missing -join ' | '))"
    Write-Host $Result.Text
    $script:failed++
  }
  foreach ($line in @($Result.Text -split "`r?`n" | Where-Object {
      $_ -match '^(OpenSlActivity: )?media4 opensl (ABI:|caller:|data:|query:|negotiation:|player:|callback trace:|volume trace:|permission:|permission trace:)|OpenSL volume boundary trace:'
    } | Select-Object -Unique)) {
    Write-Host "INFO  media4 opensl ($Abi; $Mode): $line"
  }
}

if (-not (Test-Path "$slDir/opensl-tests.dex")) {
  Write-Host 'FAIL  media4 opensl: NDK/dex artifacts are missing'
  $script:failed++
  return
}
$slAppBcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') |
  ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$slCoreVm = @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
$slAppVm = @("-Xbootclasspath:$slAppBcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
foreach ($abi in @(@{ Name = 'x86_64'; Label = 'x86_64' }, @{ Name = 'arm64-v8a'; Label = 'arm64' })) {
  $native = Join-Path $slDir $abi.Name
  $core = InvokeOpenSl ($slCoreVm + @('-cp', (NoDrive "$slDir/opensl-tests.dex"),
    'gesher.OpenSlTest', (NoDrive "$native/libopensl_contracts.so")))
  CheckOpenSl $abi.Name 'core JNI' $false $core @()
  $elf = InvokeOpenSl @('--elf', "$native/opensl-test", 'opensl-test') `
    @{ LD_LIBRARY_PATH = (NoDrive $native) } -Exe "$slHost/gesher-exec.exe"
  CheckOpenSl $abi.Name 'standalone ELF' $false $elf @() -Caller 'executable'
  foreach ($permission in @(@{ Suffix = ''; Allowed = $true; Name = 'allowed' },
                            @{ Suffix = '-denied'; Allowed = $false; Name = 'denied' })) {
    $apk = "$slHost/openslapp$($permission.Suffix)-$($abi.Label).apk"
    $single = InvokeOpenSl ($slAppVm + @('com.gesher.app.Main', (NoDrive $apk)))
    CheckOpenSl $abi.Name "single APK $($permission.Name)" $permission.Allowed $single @(
      'OpenSlActivity onResume: all passed', 'gesher-app: done')
    $deviceRoot = Join-Path $root "media4-opensl-$($abi.Name)-$($permission.Name)-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Force "$deviceRoot/data", "$deviceRoot/logs" | Out-Null
    $environment = @{ ANDROID_DATA = (NoDrive "$deviceRoot/data"); GESHER_LOGS = (NoDrive "$deviceRoot/logs") }
    try {
      $device = InvokeOpenSl ($slAppVm + @('com.gesher.app.Launcher', 'launch', (NoDrive $apk), '--headless')) $environment
      CheckOpenSl $abi.Name "device APK $($permission.Name)" $permission.Allowed $device @(
        'OpenSlActivity onResume: all passed', 'gesher-app: app process for com.gesher.opensl')
    } finally {
      $shutdown = InvokeOpenSl ($slAppVm + @('com.gesher.app.Launcher', 'shutdown')) $environment
      if ($shutdown.Code -eq 0) {
        Write-Host "PASS  media4 opensl ($($abi.Name); $($permission.Name)): device shutdown completes"
      } else {
        Write-Host "FAIL  media4 opensl ($($abi.Name); $($permission.Name)): device shutdown (exit $($shutdown.Code))"
        Write-Host $shutdown.Text
        $script:failed++
      }
    }
  }
}
