# Real AAudio policy/capability and permission-aware stream contracts (media4, ADR0064).
# No independent native-launcher backend is claimed by these JNI/APK checks.
$slHost = (Resolve-Path $Dir).Path
$slDir = Join-Path $slHost 'media/aaudio-policy'
$slExpected = @(
  'media4 aaudio exports: ok; typed API36 policy and stream identity through both ELF library paths',
  'media4 aaudio policy: ok; initial0, valid0/1/2/3 and invalid signed32 sentinels preserve the selection',
  'media4 aaudio backend: ok; UNSPECIFIED/NEVER/AUTO use real PCM legacy output, live identity survives policy changes',
  'media4 aaudio always: ok; no MMAP service returns -881/null for both directions, format validation takes priority',
  'media4 aaudio always trace: direction 0, result -881, null true',
  'media4 aaudio always trace: direction 1, result -881, null true',
  'media4 aaudio: all passed')
foreach ($policy in @(0, 1, 2, 3)) {
  foreach ($invalid in @(-2147483648, -1, 4, 2147483647)) {
    $slExpected += "media4 aaudio policy trace: selected $policy, invalid $invalid, result -898, current $policy"
  }
}
foreach ($policy in @(0, 1, 2)) {
  $slExpected += "media4 aaudio backend trace: requested $policy, result 0, mmap false, written 32, live policy 3"
}

if (-not (Test-Path "$slDir/aaudio-policy-tests.dex")) {
  Write-Host 'FAIL  media4 aaudio: NDK/dex artifacts are missing'
  $script:failed++
  return
}

# A deterministic PCM source for the real permission-aware input stream.
$slSource = Join-Path $root "media4-aaudio-source-$([guid]::NewGuid().ToString('N')).wav"
$wave = [IO.BinaryWriter]::new([IO.File]::Create($slSource))
try {
  $bytes = 44100 * 2 * 2
  $wave.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $wave.Write([int](36 + $bytes))
  $wave.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $wave.Write([int]16)
  $wave.Write([int16]1); $wave.Write([int16]2); $wave.Write([int]44100)
  $wave.Write([int](44100 * 4)); $wave.Write([int16]4); $wave.Write([int16]16)
  $wave.Write([Text.Encoding]::ASCII.GetBytes('data')); $wave.Write([int]$bytes)
  for ($frame = 0; $frame -lt 44100; $frame++) {
    $sample = [int16]([Math]::Sin(2 * [Math]::PI * 1000 * $frame / 44100) * 12000)
    $wave.Write($sample); $wave.Write($sample)
  }
} finally { $wave.Dispose() }

function InvokeAAudioPolicy([string[]] $Arguments, [hashtable] $Environment = @{}) {
  $info = [Diagnostics.ProcessStartInfo]::new("$slHost/dalvikvm.exe")
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  foreach ($a in $Arguments) { $info.ArgumentList.Add($a) }
  $info.Environment['GESHER_AUDIO'] = 'null'
  $info.Environment['GESHER_SHORTCUTS'] = '0'
  $info.Environment['GESHER_MEDIA_TRACE_RECORD'] = '1'
  $info.Environment['GESHER_AUDIO_SOURCE'] = $slSource
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

function CheckAAudioPolicy([string] $Abi, [string] $Mode, [bool] $Allowed, [hashtable] $Result, [string[]] $Extra) {
  $permission = if ($Mode -eq 'core JNI') { 'bare core without recording identity returns -889 without object' }
  elseif ($Allowed) { 'manifest allowed actual legacy capture/read/stop' } else {
    'manifest denied input returns -889 without object'
  }
  $permissionTrace = if ($Allowed) { 'expected allowed, result 0, object true' } else {
    'expected denied, result -889, object false'
  }
  if ($Allowed) {
    $Extra += 'media4 aaudio input trace: read 128, nonzero true, mmap false, live policy 3'
  }
  $missing = @(($slExpected + "media4 aaudio ABI: $Abi" + 'media4 aaudio caller: JNI' +
    "media4 aaudio permission: ok; $permission" + "media4 aaudio permission trace: $permissionTrace" + $Extra) |
    Where-Object { -not $Result.Text.Contains($_) })
  if ($Result.Code -eq 0 -and $missing.Count -eq 0 -and
      -not $Result.Text.Contains('media4 aaudio failure:')) {
    Write-Host "PASS  media4 aaudio ($Abi; $Mode): actual policy, legacy-stream identity, required-MMAP errors and recording permission"
  } else {
    Write-Host "FAIL  media4 aaudio ($Abi; $Mode) (exit $($Result.Code); missing: $($missing -join ' | '))"
    Write-Host $Result.Text
    $script:failed++
  }
  foreach ($line in @($Result.Text -split "`r?`n" | Where-Object {
      $_ -match '^(AAudioPolicyActivity: )?media4 aaudio (ABI:|caller:|exports:|policy:|policy trace:|backend:|backend trace:|always:|always trace:|input trace:|permission:|permission trace:)'
    } | Select-Object -Unique)) {
    Write-Host "INFO  media4 aaudio ($Abi; $Mode): $line"
  }
}

$slAppBcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') |
  ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$slCoreVm = @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
$slAppVm = @("-Xbootclasspath:$slAppBcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
try {
foreach ($abi in @(@{ Name = 'x86_64'; Label = 'x86_64' }, @{ Name = 'arm64-v8a'; Label = 'arm64' })) {
  $native = Join-Path $slDir $abi.Name
  $core = InvokeAAudioPolicy ($slCoreVm + @('-cp', (NoDrive "$slDir/aaudio-policy-tests.dex"),
    'gesher.AAudioPolicyTest', (NoDrive "$native/libaaudio_policy.so")))
  CheckAAudioPolicy $abi.Name 'core JNI' $false $core @()
  foreach ($permission in @(@{ Suffix = ''; Allowed = $true; Name = 'allowed' },
                            @{ Suffix = '-denied'; Allowed = $false; Name = 'denied' })) {
    $apk = "$slHost/aaudiopolicyapp$($permission.Suffix)-$($abi.Label).apk"
    $single = InvokeAAudioPolicy ($slAppVm + @('com.gesher.app.Main', (NoDrive $apk)))
    CheckAAudioPolicy $abi.Name "single APK $($permission.Name)" $permission.Allowed $single @(
      'AAudioPolicyActivity onResume: all passed', 'gesher-app: done')
    $deviceRoot = Join-Path $root "media4-aaudiopolicy-$($abi.Name)-$($permission.Name)-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Force "$deviceRoot/data", "$deviceRoot/logs" | Out-Null
    $environment = @{ ANDROID_DATA = (NoDrive "$deviceRoot/data"); GESHER_LOGS = (NoDrive "$deviceRoot/logs") }
    try {
      $device = InvokeAAudioPolicy ($slAppVm + @('com.gesher.app.Launcher', 'launch', (NoDrive $apk), '--headless')) $environment
      CheckAAudioPolicy $abi.Name "device APK $($permission.Name)" $permission.Allowed $device @(
        'AAudioPolicyActivity onResume: all passed', 'gesher-app: app process for com.gesher.aaudiopolicy')
    } finally {
      $shutdown = InvokeAAudioPolicy ($slAppVm + @('com.gesher.app.Launcher', 'shutdown')) $environment
      if ($shutdown.Code -eq 0) {
        Write-Host "PASS  media4 aaudio ($($abi.Name); $($permission.Name)): device shutdown completes"
      } else {
        Write-Host "FAIL  media4 aaudio ($($abi.Name); $($permission.Name)): device shutdown (exit $($shutdown.Code))"
        Write-Host $shutdown.Text
        $script:failed++
      }
    }
  }
}

} finally {
  Remove-Item -LiteralPath $slSource -ErrorAction Stop
}
