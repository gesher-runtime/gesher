# Media (docs/media.md, roadmap phase 8, ADR 0042). Dot-sourced by
# run-tests.ps1, whose Check, NoDrive, $Dir, $root and $failed these use;
# $root/system has framework-res.apk and build.prop.
#
# CI runners have no audio endpoint: the engine's output is captured to a WAV
# file (GESHER_AUDIO_CAPTURE: the mix, paced by the clock as a device would
# take it, with the stream volumes applied), and each sound in it is found and
# measured by media_probe analyze: its duration, its frequency (zero
# crossings) and its level. The real WASAPI path is checked to open, or to fail
# cleanly when there is no endpoint.

$mediaTmp = Join-Path $env:RUNNER_TEMP 'gesher-media'
New-Item -ItemType Directory -Force $mediaTmp | Out-Null

# The sounds of a capture, as media_probe analyze measures them.
function MediaSounds([string] $wav) {
  $out = & "$Dir/media_probe.exe" analyze $wav 2>&1 | Out-String
  $sounds = @()
  foreach ($m in [regex]::Matches($out, 'sound \d+: start ([\d.]+) s, duration ([\d.]+) s, ([\d.]+) Hz, rms ([\d.]+) ([\d.]+)')) {
    $sounds += [pscustomobject]@{
      Start = [double]$m.Groups[1].Value; Duration = [double]$m.Groups[2].Value
      Hz = [double]$m.Groups[3].Value; Rms = [math]::Max([double]$m.Groups[4].Value, [double]$m.Groups[5].Value)
    }
  }
  return @{ Text = $out; Sounds = $sounds }
}

# Compares a capture's sounds with the expected ones (Hz, seconds, rms), in
# order: frequency within 2%, duration within 60 ms, level within 15%.
function CheckSounds([string] $Name, [string] $wav, [object[]] $expected) {
  $a = MediaSounds $wav
  $sounds = $a.Sounds
  $problems = @()
  if ($sounds.Count -ne $expected.Count) { $problems += "$($sounds.Count) sounds, not $($expected.Count)" }
  for ($i = 0; $i -lt [math]::Min($sounds.Count, $expected.Count); $i++) {
    $s = $sounds[$i]; $e = $expected[$i]
    if ([math]::Abs($s.Hz - $e[0]) -gt $e[0] * 0.02) { $problems += "sound $($i + 1): $($s.Hz) Hz, not $($e[0])" }
    if ([math]::Abs($s.Duration - $e[1]) -gt 0.06) { $problems += "sound $($i + 1): $($s.Duration) s, not $($e[1])" }
    if ($e.Count -gt 2 -and [math]::Abs($s.Rms - $e[2]) -gt $e[2] * 0.15) { $problems += "sound $($i + 1): rms $($s.Rms), not $($e[2])" }
  }
  if ($problems) {
    Write-Host "FAIL  $Name ($($problems -join '; '))"
    Write-Host $a.Text
    $script:failed++
  } else {
    Write-Host "PASS  $Name"
    Write-Host (($a.Text -split "`n" | Where-Object { $_ -match '^sound ' }) -join "`n")
  }
}

# --- The engine (media_probe) ---------------------------------------------------------
$probeWav = Join-Path $mediaTmp 'probe.wav'
$env:GESHER_AUDIO_CAPTURE = $probeWav
$out = & "$Dir/media_probe.exe" tones 2>&1 | Out-String
$code = $LASTEXITCODE
Remove-Item Env:GESHER_AUDIO_CAPTURE
Check 'media: the audio engine plays 8/16/24/32-bit and float PCM, mono and stereo, 8000..96000 Hz, streaming and static tracks, a loop, markers and periodic positions (media_probe tones)' {
  $global:LASTEXITCODE = $code
  $out
} @('media_probe tones: all passed', 'output: capture to')
CheckSounds 'media: the engine''s capture has each tone at its frequency, duration and level (resampled to 48000 Hz; volume 0.5 at half the level)' $probeWav @(
  @(440, 0.6, 0.354), @(500, 0.5, 0.354), @(660, 0.5, 0.354), @(880, 0.5, 0.354),
  @(330, 0.5, 0.177), @(1200, 0.4, 0.354), @(750, 0.6, 0.354), @(520, 0.5, 0.354))

# The real output: WASAPI shared mode opens and renders, or there is no
# endpoint (CI) and that is said, not a crash.
$out = & "$Dir/media_probe.exe" wasapi 2>&1 | Out-String
$code = $LASTEXITCODE
if ($code -eq 0 -and ($out -match 'wasapi: WASAPI shared mode on .+' -and $out -match 'wasapi: rendered [1-9]\d* frames')) {
  Write-Host "PASS  media: WASAPI shared mode opens on the default endpoint and renders ($(($out -split "`n")[0].Trim()))"
} elseif ($code -eq 0 -and $out -match 'wasapi: unavailable: (WASAPI: .+)') {
  Write-Host "PASS  media: WASAPI fails cleanly where there is no audio endpoint ($($Matches[1].Trim()))"
} else {
  Write-Host "FAIL  media: WASAPI (exit $code)"
  Write-Host $out
  $failed++
}

# --- The framework: mediaapp.apk (tests/apps/mediaapp) ---------------------------------
if (Test-Path "$Dir/mediaapp.apk") {
  $bcpMedia = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
  $appCapture = Join-Path $mediaTmp 'app'
  New-Item -ItemType Directory -Force $appCapture | Out-Null
  $env:GESHER_AUDIO_CAPTURE = $appCapture
  $mediaOut = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpMedia" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' com.gesher.app.Main (NoDrive "$Dir/mediaapp.apk") 2>&1 | Out-String
  $mediaExit = $LASTEXITCODE
  Remove-Item Env:GESHER_AUDIO_CAPTURE
  $appWav = Get-ChildItem $appCapture -Filter 'audio-*.wav' | Sort-Object LastWriteTime | Select-Object -Last 1
  Check 'media: AudioTrack in mediaapp.apk: streaming (short, byte, direct ByteBuffer writes) and static tracks, 8-bit/16-bit/float, mono and stereo, a marker, periodic notifications, loop points, setPlaybackRate, getTimestamp' {
    $global:LASTEXITCODE = $mediaExit
    $mediaOut
  } @('MediaTest audiotrack output: native rate 48000, min buffer (44100 mono 16-bit) ok, property rate 48000, frames per buffer ok',
      'MediaTest audiotrack 1 streaming 16-bit mono 44100: state 1, stream 3, wrote 22050, position 22050/22050, timestamp true, session ok',
      'MediaTest audiotrack 2 streaming 8-bit mono 8000: wrote 3200, position 3200/3200',
      'MediaTest audiotrack 3 static float stereo 48000: states 2->1, wrote 38400, marker true at 9600, position 19200/19200',
      'MediaTest audiotrack 4 bytebuffer 16-bit stereo 22050 volume 0.5: wrote 35280, position 8820/8820, periodic notifications 4, underruns ok',
      'MediaTest audiotrack 5 legacy static loop: stream 3, wrote 12800, setLoopPoints 0',
      'MediaTest audiotrack 6 playback rate: set 0, rate 48000, wrote 14400, position 14400/14400',
      'MediaTest done', 'gesher-app: done')
  if ($null -eq $appWav) {
    Write-Host "FAIL  media: mediaapp.apk's audio output was not captured"
    $failed++
  } else {
    CheckSounds 'media: mediaapp.apk''s AudioTracks reach the output: each tone at its frequency, duration and level' $appWav.FullName @(
      @(440, 0.5, 0.354), @(500, 0.4, 0.354), @(660, 0.4, 0.354), @(880, 0.4, 0.177),
      @(1000, 0.6, 0.354), @(1200, 0.3, 0.354))
  }
} else {
  Write-Host 'INFO  media: mediaapp.apk not in the build output; its checks skipped'
}
