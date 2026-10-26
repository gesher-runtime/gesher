# Recording (media3: docs/media.md "Recording", ADR 0059). Dot-sourced by run-tests.ps1,
# whose Check, NoDrive, $Dir, $root and $failed these use.
#
# CI runners have no microphone: the capture engine's source is a WAV file
# (GESHER_AUDIO_SOURCE), a second of 750 Hz for 0.4 s, silence, 1250 Hz for 0.4 s,
# silence, looped from its start each time recording starts; what is recorded is found
# and measured (media_probe analyze, or the test app's own zero crossings). The real
# WASAPI capture path is checked to open, or to give silence when there is no endpoint.

$m3Tmp = Join-Path $env:RUNNER_TEMP 'gesher-media3'
New-Item -ItemType Directory -Force $m3Tmp | Out-Null

# The microphone's file: 48000 Hz mono 16-bit.
function WriteMicWav([string] $path) {
  $rate = 48000
  $samples = New-Object 'System.Int16[]' $rate
  for ($i = 0; $i -lt $rate; $i++) {
    $t = $i / $rate
    $v = 0.0
    if ($t -lt 0.4) { $v = 0.5 * [math]::Sin(2 * [math]::PI * 750 * $t) }
    elseif ($t -ge 0.5 -and $t -lt 0.9) { $v = 0.5 * [math]::Sin(2 * [math]::PI * 1250 * ($t - 0.5)) }
    $samples[$i] = [int16][math]::Round($v * 32767)
  }
  $ms = [IO.MemoryStream]::new()
  $w = [IO.BinaryWriter]::new($ms)
  $data = $rate * 2
  $w.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $w.Write([int](36 + $data))
  $w.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $w.Write([int]16); $w.Write([int16]1); $w.Write([int16]1)
  $w.Write([int]$rate); $w.Write([int]($rate * 2)); $w.Write([int16]2); $w.Write([int16]16)
  $w.Write([Text.Encoding]::ASCII.GetBytes('data')); $w.Write([int]$data)
  foreach ($s in $samples) { $w.Write($s) }
  $w.Flush()
  [IO.File]::WriteAllBytes($path, $ms.ToArray())
}
$micWav = Join-Path $m3Tmp 'mic.wav'
WriteMicWav $micWav

# Each regex must match a line of $Out (multi-line).
function LineCheck([string] $Name, [string] $Out, [string[]] $Patterns, [int] $Code = 0, [string] $Tag = '') {
  $missing = @($Patterns | Where-Object { -not [regex]::IsMatch($Out, $_, 'Multiline') })
  if ($Code -eq 0 -and $missing.Count -eq 0) {
    Write-Host "PASS  $Name"
    return
  }
  Write-Host "FAIL  $Name (exit $Code; missing: $($missing -join ' | '))"
  $script:failed++
  if ($Tag -and -not $script:m3Printed[$Tag]) {
    $script:m3Printed[$Tag] = $true
    ($Out -split "`n" | Where-Object { $_ -match 'RecordTest|GesherMedia|gesher-app|Exception|FATAL| F dalvikvm|JNI DETECTED| at ' } |
      Select-Object -First 200) | Out-Host
  }
}
$script:m3Printed = @{}

# --- The capture engine (media_probe capture) --------------------------------------------
Remove-Item Env:GESHER_AUDIO_SOURCE -ErrorAction SilentlyContinue
$out = & "$Dir/media_probe.exe" capture 2>&1 | Out-String
$code = $LASTEXITCODE
if ($out -match 'capture: (WASAPI shared mode capture on [^\r\n]+)') {
  Write-Host "PASS  media3: the capture engine records from the default capture endpoint ($($Matches[1].Trim()))"
} elseif ($code -eq 0 -and $out -match 'read 88200 frames' -and $out -match 'capture: (none: [^\r\n]+)') {
  Write-Host "PASS  media3: the capture engine without a capture endpoint records silence, paced by the clock ($($Matches[1].Trim()))"
} else {
  Write-Host "FAIL  media3: the capture engine without GESHER_AUDIO_SOURCE (exit $code)"
  Write-Host $out
  $failed++
}
$env:GESHER_AUDIO_SOURCE = $micWav
$probeRec = Join-Path $m3Tmp 'probe-capture.wav'
$out = & "$Dir/media_probe.exe" capture $probeRec 2>&1 | Out-String
$code = $LASTEXITCODE
Check 'media3: the capture engine records GESHER_AUDIO_SOURCE''s file at 44100 Hz stereo (resampled from 48000 Hz mono), 2 s paced by the clock' {
  $global:LASTEXITCODE = $code
  $out
} @('capture: file ', '(48000 Hz, 1 channels, 1000 ms, looping)', 'capture: read 88200 frames')
$a = MediaSounds $probeRec
$want = @(@(750, 0.4), @(1250, 0.4), @(750, 0.4), @(1250, 0.4))
$problems = @()
if ($a.Sounds.Count -ne 4) { $problems += "$($a.Sounds.Count) sounds, not 4" }
for ($i = 0; $i -lt [math]::Min($a.Sounds.Count, 4); $i++) {
  $s = $a.Sounds[$i]
  if ([math]::Abs($s.Hz - $want[$i][0]) -gt $want[$i][0] * 0.02) { $problems += "sound $($i + 1): $($s.Hz) Hz" }
  if ([math]::Abs($s.Duration - $want[$i][1]) -gt 0.06) { $problems += "sound $($i + 1): $($s.Duration) s" }
  if ([math]::Abs($s.Rms - 0.354) -gt 0.354 * 0.15) { $problems += "sound $($i + 1): rms $($s.Rms)" }
}
if ($problems) {
  Write-Host "FAIL  media3: what the capture engine recorded is the source's tones ($($problems -join '; '))"
  Write-Host $a.Text
  $failed++
} else {
  Write-Host 'PASS  media3: what the capture engine recorded is the source''s tones: 750 Hz, 1250 Hz, 750 Hz, 1250 Hz, 0.4 s each, at their level'
}

# --- recordapp.apk (tests/apps/recordapp) ------------------------------------------------
# The expected lines, both modes. The microphone is GESHER_AUDIO_SOURCE's file; the camera
# GESHER_CAMERA_TEST's; the app's sounds (none) go to a capture file.
$t = 'RecordTest '
$recordChecks = @(
  @{ Name = 'MediaSession over the session service: info, metadata, playback state, queue, extras, controller transport and update callbacks, active sessions and release'
     P = @("${t}media session: info, metadata, state, queue, extras, play/pause/seek callbacks, active sessions and release") },
  @{ Name = 'AudioManager lists the input device (GESHER_AUDIO_SOURCE''s, as a built-in microphone)'
     P = @("${t}devices: 1 input\(s\) \[type 15, source true, name Gesher audio source\]") },
  @{ Name = 'AudioRecord: 16-bit mono 44100 Hz (getMinBufferSize, states, blocking reads, getTimestamp, a marker, periodic notifications, the routed device) and float stereo 48000 Hz through the Builder; what it read is the microphone''s tones at their level'
     P = @("${t}audiorecord 16-bit mono 44100: min buffer ok, state 1, recording 3, read 44100 frames, 750 Hz then 1250 Hz, level ok, timestamp true, marker true, periodic 8\+, routed to type 15",
           "${t}audiorecord float stereo 48000 \(builder\): channels 2, read 48000 frames, 750 Hz then 1250 Hz") },
  @{ Name = 'AAudio input (a data callback stream, a blocking read) and an OpenSL ES recorder (an Android simple buffer queue) from an app''s NDK library: the microphone''s tones'
     P = @("${t}ndk aaudio input callback: direction input, rate 48000, 48000 frames, callbacks 50\+, frames read all, 750 Hz then 1250 Hz",
           "${t}ndk aaudio input read: stereo 16-bit 44100, read 44100 frames, 750 Hz then 1250 Hz",
           "${t}ndk opensl recorder: state recording, 16000 frames, 10 buffers, position 1 s, 750 Hz then 1250 Hz") },
  @{ Name = 'MediaCodecList lists the encoders (H.264, AAC, Opus)'
     P = @("${t}codec list: avc encoder c2\.gesher\.avc\.encoder, aac encoder c2\.gesher\.aac\.encoder, opus encoder c2\.gesher\.opus\.encoder") },
  @{ Name = 'MediaCodec encoders and MediaMuxer: AAC into MPEG-4, Opus into WebM (the codec-config buffers, csd-0), read back by MediaExtractor and decoded to the tone'
     P = @("${t}encoder aac: c2\.gesher\.aac\.encoder, csd-0 2 bytes, config buffers 1, 40\+ packets; MPEG-4 by MediaMuxer: 1 track\(s\) \[audio/mp4a-latm 44100 Hz 2 ch\], 1\.\d s; decoded 1000 Hz",
           "${t}encoder opus: c2\.gesher\.opus\.encoder, OpusHead true, 40\+ packets; WebM by MediaMuxer: 1 track\(s\) \[audio/opus 48000 Hz 1 ch\], (0\.9|1\.\d) s; decoded 1000 Hz") },
  @{ Name = 'MediaCodec''s H.264 encoder from ByteBuffers and Images, and from an input Surface (Canvas): the parameter sets as csd-0/csd-1, key frames; MediaMuxer''s MPEG-4 decodes back to every color in order'
     P = @("${t}encoder h264 \(buffers and images\): c2\.gesher\.avc\.encoder, csd-0 SPS true, csd-1 PPS true, config buffers 1, 60 frames, key frames 2\+; MPEG-4 by MediaMuxer: 1 track\(s\) \[video/avc 320x240\], [12]\.\d s; decoded 60 frames, ordered true, colors \[red, lime, blue, white\]",
           "${t}encoder h264 \(input surface\): c2\.gesher\.avc\.encoder, 50\+ frames; MPEG-4 by MediaMuxer: 1 track\(s\) \[video/avc 320x240\], [12]\.\d s; decoded 50\+ frames, ordered true, colors \[red, lime, blue, white\]") },
  @{ Name = 'MediaMuxer: an MPEG-4 of H.264 and AAC, read by MediaExtractor and MediaMetadataRetriever (duration, the frame at 1.25 s)'
     P = @("${t}muxer mp4 \(video and audio\): 2 track\(s\) \[video/avc 320x240, audio/mp4a-latm 48000 Hz 1 ch\], 2\.\d s; retriever duration ok, frame at 1\.25 s 320x240 blue; audio 1000 Hz") },
  @{ Name = 'MediaRecorder, audio: AAC in MPEG-4 and ADTS, Opus in Ogg and WebM, getMaxAmplitude; each file read back has the microphone''s tones'
     P = @("${t}recorder aac mpeg4: max amplitude loud, 1 track\(s\) \[audio/mp4a-latm 44100 Hz 1 ch\], 1\.\d s, decoded 750 Hz then 1250 Hz",
           "${t}recorder opus ogg: max amplitude loud, 1 track\(s\) \[audio/opus 48000 Hz 1 ch\], [\d.]+ s, decoded 750 Hz then 1250 Hz",
           "${t}recorder opus webm: max amplitude loud, 1 track\(s\) \[audio/opus 48000 Hz 1 ch\], 1\.\d s, decoded 750 Hz then 1250 Hz",
           "${t}recorder aac adts: max amplitude loud, 1 track\(s\) \[audio/mp4a-latm 48000 Hz 1 ch\], [\d.]+ s, decoded 750 Hz then 1250 Hz") },
  @{ Name = 'MediaRecorder, video from its Surface (Canvas) with the microphone: H.264 and AAC in MPEG-4, every color in order, the frame at 1.25 s, the tones'
     P = @("${t}recorder surface: getSurface the same true, 2 track\(s\) \[video/avc 320x240, audio/mp4a-latm 44100 Hz 1 ch\], [12]\.\d s; video 45\+ frames, colors \[red, lime, blue, white\], frame at 1\.25 s 320x240 blue; audio 750 Hz then 1250 Hz") },
  @{ Name = 'MediaRecorder, video from the camera (Camera2: GESHER_CAMERA_TEST''s camera, a repeating request into getSurface()): H.264 in MPEG-4 with the test picture'
     P = @("${t}recorder camera \d+: 1 track\(s\) \[video/avc 320x240\], [12]\.\d s; video 30\+ frames, ordered true, the test picture true") },
  @{ Name = 'MediaCodec''s output into an ImageReader''s Surface (YUV_420_888 Images): every color in order'
     P = @("${t}imagereader: decoder rendered 60 frames into the reader's surface; images 40\+ \(35 320x240 3 planes\), colors \[red, lime, blue, white\]") }
)
function RecordChecks([string] $Mode, [string] $Out, [int] $Code) {
  foreach ($c in $recordChecks) {
    LineCheck "media3 ($Mode): $($c.Name)" $Out ($c.P + @("^${t}done")) $Code "record-$Mode"
  }
}

if (Test-Path "$Dir/recordapp.apk") {
  $bcpRec = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
  $dvmRec = @("-Xbootclasspath:$bcpRec", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
  $env:GESHER_CAMERA_TEST = '1'
  $env:GESHER_AUDIO_CAPTURE = Join-Path $m3Tmp 'out-single'
  New-Item -ItemType Directory -Force $env:GESHER_AUDIO_CAPTURE | Out-Null
  # Single process.
  $recOut = & "$Dir/dalvikvm.exe" @dvmRec com.gesher.app.Main (NoDrive "$Dir/recordapp.apk") 2>&1 | Out-String
  $recCode = $LASTEXITCODE
  Write-Host "INFO  media3: $((($recOut -split "`n") | Where-Object { $_ -match 'GesherMedia.*(encoders:|capture:)|RecordTest codec list encoders' } | Select-Object -First 4 | ForEach-Object { $_.Trim() }) -join '; ')"
  RecordChecks 'single process' $recOut $recCode

  # The Gesher device: the app in a process of its own (logs/com.gesher.recordapp.log), its
  # environment the device's (GESHER_AUDIO_SOURCE, GESHER_CAMERA_TEST).
  $env:GESHER_AUDIO_CAPTURE = Join-Path $m3Tmp 'out-device'
  New-Item -ItemType Directory -Force $env:GESHER_AUDIO_CAPTURE | Out-Null
  $launcherRec = $dvmRec + 'com.gesher.app.Launcher'
  $null = & "$Dir/dalvikvm.exe" @($launcherRec + 'shutdown') 2>&1 | Out-String
  $devOut = & "$Dir/dalvikvm.exe" @($launcherRec + 'launch', (NoDrive "$Dir/recordapp.apk"), '--headless', '--keep-task') 2>&1 | Out-String
  $devCode = $LASTEXITCODE
  $devOut += "`n" + (Get-Content (Join-Path $root 'logs/com.gesher.recordapp.log') -Raw -ErrorAction SilentlyContinue)
  RecordChecks 'Gesher device' $devOut $devCode
  $null = & "$Dir/dalvikvm.exe" @($launcherRec + 'shutdown') 2>&1 | Out-String
  Remove-Item Env:GESHER_CAMERA_TEST, Env:GESHER_AUDIO_CAPTURE -ErrorAction SilentlyContinue
} else {
  Write-Host 'INFO  media3: recordapp.apk not in the build output; its checks skipped'
}
Remove-Item Env:GESHER_AUDIO_SOURCE -ErrorAction SilentlyContinue

# videoapp.apk asks for no RECORD_AUDIO (38-media.ps1 ran it): recording is refused.
if ($null -ne $videoOut) {
  Check 'media3: without RECORD_AUDIO recording is refused (videoapp.apk): the AudioRecord stays uninitialized, startRecording throws, MediaRecorder.setAudioSource throws' {
    $global:LASTEXITCODE = 0
    $videoOut
  } @('VideoTest record without RECORD_AUDIO: AudioRecord state 0, startRecording IllegalStateException, MediaRecorder.setAudioSource RuntimeException')
}
