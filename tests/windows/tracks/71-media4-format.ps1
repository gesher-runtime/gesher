# AMediaFormat through the real NDK ELF ABI (media4, ADR 0064), with the same
# fixture in dalvikvm, an independent executable and both installed APK modes.
$mfDir = Join-Path (Resolve-Path $Dir).Path 'media/ndk-format'
$mfHost = (Resolve-Path $Dir).Path
$mfExpected = @(
  'media4 format exports: ok; 21 functions through bare/system/default handles; no dummy codec',
  'media4 format data: ok; 152 actual pointer objects and pinned AOSP values; ELF data indirection',
  'media4 format typed: ok; int32/int64/float/double/LP64 size/rectangle; direct and dlsym calls',
  'media4 format ownership: ok; UTF-8 and binary input copies; copied values survive source deletion',
  'media4 format errors: ok; missing/type mismatch preserve outputs; pinned null/copy/delete results',
  'media4 format clear: ok; clear removes old entries and permits typed reuse',
  'media4 format dump: ok; typed debug output and the pinned borrowed-string lifetime',
  'media4 format lifecycle: ok; 128 independent new/copy/clear/delete cycles',
  'media4 format: all passed')

function InvokeMediaFormat([string] $Exe, [string[]] $Arguments, [hashtable] $Environment = @{}) {
  $info = [Diagnostics.ProcessStartInfo]::new($Exe)
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  foreach ($a in $Arguments) { $info.ArgumentList.Add($a) }
  foreach ($k in $Environment.Keys) { $info.Environment[$k] = $Environment[$k] }
  $process = [Diagnostics.Process]::Start($info)
  $stdout = $process.StandardOutput.ReadToEndAsync()
  $stderr = $process.StandardError.ReadToEndAsync()
  $timedOut = -not $process.WaitForExit(120000)
  if ($timedOut) { $process.Kill($true); $process.WaitForExit() }
  $text = ''
  if ($stdout.Wait(5000)) { $text += $stdout.Result } else { $timedOut = $true }
  if ($stderr.Wait(5000)) { $text += $stderr.Result } else { $timedOut = $true }
  if ($timedOut) { $text += '(media4 format fixture timed out)' }
  $code = if ($timedOut) { -1 } else { $process.ExitCode }
  $process.Dispose()
  return @{ Text = $text; Code = $code }
}

function CheckMediaFormat([string] $Abi, [string] $Mode, [hashtable] $Result, [string[]] $Extra) {
  $missing = @(($mfExpected + "media4 format ABI: $Abi" + $Extra) |
    Where-Object { -not $Result.Text.Contains($_) })
  if ($Result.Code -eq 0 -and $missing.Count -eq 0 -and
      -not $Result.Text.Contains('media4 format failure:')) {
    Write-Host "PASS  media4 format ($Abi; $Mode): actual NDK functions/data, typed values, ownership, errors and lifecycle"
  } else {
    Write-Host "FAIL  media4 format ($Abi; $Mode) (exit $($Result.Code); missing: $($missing -join ' | '))"
    Write-Host $Result.Text
    $script:failed++
  }
  foreach ($line in @($Result.Text -split "`r?`n" | Where-Object {
      $_ -match '^(MediaFormatActivity: )?media4 format (ABI:|caller:|exports:|data:|typed:|ownership:|errors:|clear:|dump:|lifecycle:)'
    } | Select-Object -Unique)) {
    Write-Host "INFO  media4 format ($Abi; $Mode): $line"
  }
}

if (-not (Test-Path "$mfDir/mediaformat-tests.dex")) {
  Write-Host 'FAIL  media4 format: NDK/dex artifacts are missing'
  $script:failed++
  return
}

$mfAppBcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') |
  ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$mfCoreVm = @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
$mfAppVm = @("-Xbootclasspath:$mfAppBcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
foreach ($abi in @(@{ Name = 'x86_64'; Apk = 'mediaformatapp-x86_64.apk' },
                    @{ Name = 'arm64-v8a'; Apk = 'mediaformatapp-arm64.apk' })) {
  $native = Join-Path $mfDir $abi.Name
  $dvm = InvokeMediaFormat "$mfHost/dalvikvm.exe" ($mfCoreVm + @('-cp',
    (NoDrive "$mfDir/mediaformat-tests.dex"), 'gesher.MediaFormatTest',
    (NoDrive "$native/libmediaformat_test.so")))
  CheckMediaFormat $abi.Name 'dalvikvm JNI' $dvm @('media4 format caller: JNI')

  $elf = InvokeMediaFormat "$mfHost/gesher-exec.exe" @('--elf',
    "$native/mediaformat-test", 'mediaformat-test') @{ LD_LIBRARY_PATH = (NoDrive $native) }
  CheckMediaFormat $abi.Name 'standalone ELF' $elf @('media4 format caller: executable')

  $single = InvokeMediaFormat "$mfHost/dalvikvm.exe" ($mfAppVm + @(
    'com.gesher.app.Main', (NoDrive "$mfHost/$($abi.Apk)")))
  CheckMediaFormat $abi.Name 'single-process APK' $single @(
    'MediaFormatActivity onResume: all passed', 'media4 format caller: JNI', 'gesher-app: done')

  # A fresh data/log root per ABI prevents either prior APK's cached output
  # from satisfying the device checks, in addition to the native ABI marker.
  $deviceRoot = Join-Path $root "media4-format-$($abi.Name)-$([guid]::NewGuid().ToString('N'))"
  New-Item -ItemType Directory -Force "$deviceRoot/data", "$deviceRoot/logs" | Out-Null
  $environment = @{
    ANDROID_DATA = (NoDrive "$deviceRoot/data")
    GESHER_LOGS = (NoDrive "$deviceRoot/logs")
    GESHER_SHORTCUTS = '0'
  }
  try {
    $device = InvokeMediaFormat "$mfHost/dalvikvm.exe" ($mfAppVm + @(
      'com.gesher.app.Launcher', 'launch', (NoDrive "$mfHost/$($abi.Apk)"), '--headless')) $environment
    CheckMediaFormat $abi.Name 'device APK' $device @('media4 format caller: JNI',
      'MediaFormatActivity onResume: all passed', 'gesher-app: app process for com.gesher.mediaformat')
  } finally {
    $shutdown = InvokeMediaFormat "$mfHost/dalvikvm.exe" ($mfAppVm + @(
      'com.gesher.app.Launcher', 'shutdown')) $environment
    if ($shutdown.Code -ne 0) {
      Write-Host "FAIL  media4 format ($($abi.Name)): device shutdown (exit $($shutdown.Code))"
      Write-Host $shutdown.Text
      $script:failed++
    } else {
      Write-Host "PASS  media4 format ($($abi.Name)): device shutdown completes"
    }
  }
}
