# Real NDK muxer contracts and independent AOSP container/decoder verification.
# All compilation and native/JVM/C# execution are CI-only.
$mxDir = "$Dir/media/ndk-muxer"
$mxBcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') |
  ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$mxCore = @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
$mxApp = @("-Xbootclasspath:$mxBcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')

function InvokeMuxer([string] $Exe, [string[]] $Arguments, [hashtable] $Environment, [int] $Seconds = 180) {
  $info = [Diagnostics.ProcessStartInfo]::new((Resolve-Path $Exe).Path)
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  # Arguments and the image's embedded dist/*.jar locations are relative to
  # the caller, just as in the full suite. Changing to dist makes them dist/dist.
  $info.WorkingDirectory = (Get-Location).ProviderPath
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
  foreach ($key in $Environment.Keys) { $info.Environment[$key] = $Environment[$key] }
  $process = [Diagnostics.Process]::Start($info)
  $text = ''
  $timedOut = $false
  try {
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($Seconds * 1000)) {
      $timedOut = $true
      $text += "process exceeded $Seconds seconds`n"
      try { $process.Kill($true) } catch { $text += "process kill failed: $_`n" }
      if (-not $process.WaitForExit(5000)) { $text += "process still alive five seconds after kill`n" }
    }
    foreach ($pipe in @(@{ Task = $stdout; Name = 'stdout' }, @{ Task = $stderr; Name = 'stderr' })) {
      try {
        if ($pipe.Task.Wait(5000)) { $text += $pipe.Task.Result }
        else { $timedOut = $true; $text += "$($pipe.Name) did not finish within five seconds`n" }
      } catch { $timedOut = $true; $text += "$($pipe.Name) read failed: $_`n" }
    }
    return @{ Text = $text; Code = $(if ($timedOut) { 124 } else { $process.ExitCode }) }
  } finally { $process.Dispose() }
}

function CheckMuxer([string] $Abi, [string] $Mode, [string] $Caller, $Result, [string] $Output) {
  # Java console and launcher forwarding can produce CRLF or CRCRLF. Keep
  # every actual record and required count while normalizing only CR bytes.
  $text = $Result.Text.Replace("`r", '')
  $missing = @(@("media4 muxer ABI: $Abi", "media4 muxer caller: $Caller", 'media4 muxer: all passed',
    'media4 muxer descriptor-access-ownership-seekability: ok', 'media4 muxer empty-lifecycle-and-error-order: ok',
    'media4 muxer Ogg boundary: packets 0 result=ok', 'media4 muxer Ogg boundary: packets 1 result=ok',
    'media4 muxer Ogg boundary: packets 2 result=ok payload-eos=1') |
    Where-Object { -not $text.Contains($_) })
  $parts = @([regex]::Matches($text, '(?m)^(?:MediaMuxerActivity: )?media4 muxer [^\r\n]+: ok\r?$'))
  $fds = @([regex]::Matches($text, '(?m)^(?:MediaMuxerActivity: )?media4 muxer fd trace: [^\r\n]+$'))
  $outputs = @([regex]::Matches($text, '(?m)^(?:MediaMuxerActivity: )?media4 muxer output: [^\r\n]+$'))
  if ($Result.Code -eq 0 -and $missing.Count -eq 0 -and $parts.Count -eq 24 -and
      $fds.Count -eq 12 -and $outputs.Count -eq 14 -and -not $text.Contains('FAILED')) {
    Write-Host "PASS  media4 muxer ($Abi; $Mode): ten typed APIs, actual FD modes, retained track ownership, sample offsets, state errors and fourteen encoded containers including delete-active and shorter audio"
  } else {
    Write-Host "FAIL  media4 muxer ($Abi; $Mode): exit $($Result.Code), parts $($parts.Count)/24, FD controls $($fds.Count)/12, outputs $($outputs.Count)/14, missing $($missing -join ' | ')"
    Write-Host $text
    $script:failed++
  }
  foreach ($line in @($text -split "`r?`n" | Where-Object { $_ -match 'media4 muxer (ABI:|caller:|Ogg boundary:|fd trace:|invalid-fd trace:|unsupported trace:|empty trace:|output:|write failure:|[^\r\n]+: ok)' })) {
    Write-Host "INFO  media4 muxer ($Abi; $Mode): $line"
  }
  $verification = InvokeMuxer "$Dir/media_muxer_verify.exe" @((NoDrive $Output), (NoDrive "$mxDir/fixtures")) @{} 180
  $containers = @([regex]::Matches($verification.Text, '(?m)^media4 muxer verify actual-container-decode [^\r\n]+: ok\r?$'))
  if ($verification.Code -eq 0 -and $containers.Count -eq 14 -and
      $verification.Text.Contains('media4 muxer independent verification: 0 failure(s)')) {
    Write-Host "PASS  media4 muxer ($Abi; $Mode): independent AOSP extractors decode fourteen containers, compare WebM packets and PTS, seek and decode, rotation and ISO-6709 location"
  } else {
    Write-Host "FAIL  media4 muxer ($Abi; $Mode): independent output verification exit $($verification.Code), containers $($containers.Count)/14"
    $script:failed++
  }
  foreach ($line in @($verification.Text -split "`r?`n" | Where-Object { $_ })) {
    Write-Host "INFO  media4 muxer ($Abi; $Mode): $line"
  }
  $cueAudit = InvokeMuxer (Get-Command python).Source @(
    "$PSScriptRoot/../../media/ndk-muxer/verify-webm-cues.py", (NoDrive $Output)) @{} 30
  Write-Host $cueAudit.Text
  if ($cueAudit.Code -eq 0 -and $cueAudit.Text.Contains('webm cue verification: 0 failures')) {
    Write-Host "PASS  media4 muxer ($Abi; $Mode): WebM cue points address video clusters"
  } else {
    Write-Host "FAIL  media4 muxer ($Abi; $Mode): WebM cue structure exit $($cueAudit.Code)"
    $script:failed++
  }
  $decodedOgg = [regex]::Match($verification.Text, 'audio trace: [^\r\n]*opus-interleaved\.ogg; [^\r\n]*frames (\d+)')
  $decodedFrames = if ($decodedOgg.Success) { $decodedOgg.Groups[1].Value } else { '-1' }
  $packetAudit = InvokeMuxer (Get-Command python).Source @(
    "$PSScriptRoot/../../media/ndk-muxer/verify-ogg-packets.py",
    '--fixture', "$mxDir/fixtures/opus.json", '--output', "$Output/opus-interleaved.ogg", '--decoded-frames', $decodedFrames) @{} 30
  Write-Host $packetAudit.Text
  if ($packetAudit.Code -eq 0 -and $packetAudit.Text.Contains('media4 muxer Ogg packet verification: 0 failure(s)')) {
    Write-Host "PASS  media4 muxer ($Abi; $Mode): actual Ogg audio packets match every independent source packet with no empty EOS packet"
  } else {
    Write-Host "FAIL  media4 muxer ($Abi; $Mode): independent Ogg packet boundary/hash verification exit $($packetAudit.Code)"
    $script:failed++
  }
  foreach ($count in 0, 1, 2) {
    $boundaryAudit = InvokeMuxer (Get-Command python).Source @(
      "$PSScriptRoot/../../media/ndk-muxer/verify-ogg-packets.py", '--fixture', "$mxDir/fixtures/opus.json",
      '--output', "$Output/opus-boundary-$count.ogg", '--limit', "$count") @{} 30
    Write-Host $boundaryAudit.Text
    if ($boundaryAudit.Code -eq 0 -and $boundaryAudit.Text.Contains('media4 muxer Ogg packet verification: 0 failure(s)')) {
      Write-Host "PASS  media4 muxer ($Abi; $Mode): actual $count-packet Ogg EOS, source payload order, granules and independent CRC"
    } else {
      Write-Host "FAIL  media4 muxer ($Abi; $Mode): actual $count-packet Ogg boundary verification exit $($boundaryAudit.Code)"
      $script:failed++
    }
  }
}

if (-not (Test-Path "$mxDir/mediamuxer-tests.dex") -or -not (Test-Path "$Dir/media_muxer_verify.exe")) {
  Write-Host 'FAIL  media4 muxer: native/dex/oracle artifacts are missing'
  $script:failed++
  return
}
foreach ($abi in @(@{ Name = 'x86_64'; Label = 'x86_64' }, @{ Name = 'arm64-v8a'; Label = 'arm64' })) {
  foreach ($mode in @('core JNI', 'standalone ELF', 'single APK', 'device APK')) {
    $caseRoot = Join-Path $root "ndk-muxer-$($abi.Name)-$([guid]::NewGuid().ToString('N'))"
    $output = "$caseRoot/output"
    New-Item -ItemType Directory -Force $output, "$caseRoot/data", "$caseRoot/logs" | Out-Null
    $environment = @{ GESHER_MUXER_FIXTURES = (NoDrive "$mxDir/fixtures"); GESHER_MUXER_OUT = (NoDrive $output) }
    $native = "$mxDir/$($abi.Name)"
    $caller = 'JNI'
    if ($mode -eq 'core JNI') {
      $result = InvokeMuxer "$Dir/dalvikvm.exe" ($mxCore + @('-cp', (NoDrive "$mxDir/mediamuxer-tests.dex"),
        'gesher.MediaMuxerTest', (NoDrive (Resolve-Path "$native/libmediamuxer_test.so").Path))) $environment
    } elseif ($mode -eq 'standalone ELF') {
      $environment['LD_LIBRARY_PATH'] = NoDrive $native
      $caller = 'executable'
      $result = InvokeMuxer "$Dir/gesher-exec.exe" @('--elf', "$native/ndk-muxer-test", 'ndk-muxer-test') $environment
    } elseif ($mode -eq 'single APK') {
      $result = InvokeMuxer "$Dir/dalvikvm.exe" ($mxApp + @('com.gesher.app.Main',
        (NoDrive "$Dir/mediamuxerapp-$($abi.Label).apk"))) $environment
    } else {
      $environment['ANDROID_DATA'] = NoDrive "$caseRoot/data"
      $environment['GESHER_LOGS'] = NoDrive "$caseRoot/logs"
      try {
        $result = InvokeMuxer "$Dir/dalvikvm.exe" ($mxApp + @('com.gesher.app.Launcher', 'launch',
          (NoDrive "$Dir/mediamuxerapp-$($abi.Label).apk"), '--headless')) $environment
      } finally {
        $shutdown = InvokeMuxer "$Dir/dalvikvm.exe" ($mxApp + @('com.gesher.app.Launcher', 'shutdown')) $environment 30
        if ($shutdown.Code -eq 0) { Write-Host "PASS  media4 muxer ($($abi.Name)): device shutdown completes" }
        else {
          Write-Host "FAIL  media4 muxer ($($abi.Name)): device shutdown exit $($shutdown.Code): $($shutdown.Text)"
          $script:failed++
        }
      }
    }
    if ($mode -match 'APK') {
      if (-not $result.Text.Contains('MediaMuxerActivity onResume: all passed')) {
        $result.Text += "`nFAILED: APK did not report onResume completion"
      }
    }
    CheckMuxer $abi.Name $mode $caller $result $output
  }
}
