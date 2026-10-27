# Real OpenSL identifiers/query/negotiation/permission contracts (media4, ADR0064).
# No independent native-launcher backend is claimed by these JNI/APK checks.
$slHost = (Resolve-Path $Dir).Path
$slDir = Join-Path $slHost 'media/opensl-contracts'
$slExpected = @(
  'media4 opensl data: ok; 53 canonical UUID pointer objects, exact pinned fields and ELF indirection',
  'media4 opensl query: ok; typed global and engine queries, supported subsets, null/index/object controls',
  'media4 opensl negotiation: ok; copied UUIDs, all 53 required/optional engine outcomes, duplicate and invalid arrays',
  'media4 opensl player: ok; real PCM queue and volume, explicit exposure and unsupported effect rejection',
  'media4 opensl: all passed')

function InvokeOpenSl([string[]] $Arguments, [hashtable] $Environment = @{}) {
  $info = [Diagnostics.ProcessStartInfo]::new("$slHost/dalvikvm.exe")
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  foreach ($a in $Arguments) { $info.ArgumentList.Add($a) }
  $info.Environment['GESHER_AUDIO'] = 'null'
  $info.Environment['GESHER_SHORTCUTS'] = '0'
  foreach ($key in $Environment.Keys) { $info.Environment[$key] = $Environment[$key] }
  $process = [Diagnostics.Process]::Start($info)
  $stdout = $process.StandardOutput.ReadToEndAsync()
  $stderr = $process.StandardError.ReadToEndAsync()
  $timedOut = -not $process.WaitForExit(120000)
  if ($timedOut) { $process.Kill($true); $process.WaitForExit() }
  $text = ''
  if ($stdout.Wait(5000)) { $text += $stdout.Result } else { $timedOut = $true }
  if ($stderr.Wait(5000)) { $text += $stderr.Result } else { $timedOut = $true }
  $code = if ($timedOut) { -1 } else { $process.ExitCode }
  $process.Dispose()
  return @{ Text = $text; Code = $code }
}

function CheckOpenSl([string] $Abi, [string] $Mode, [bool] $Allowed, [hashtable] $Result, [string[]] $Extra) {
  $permission = if ($Allowed) { 'manifest allowed native recorder lifecycle' } else {
    'manifest denied native recorder returns 0x0B without object'
  }
  $missing = @(($slExpected + "media4 opensl ABI: $Abi" + 'media4 opensl caller: JNI' +
    "media4 opensl permission: ok; $permission" + $Extra) |
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
      $_ -match '^(OpenSlActivity: )?media4 opensl (ABI:|caller:|data:|query:|negotiation:|player:|permission:)'
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
  CheckOpenSl $abi.Name 'core JNI' $true $core @()
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
