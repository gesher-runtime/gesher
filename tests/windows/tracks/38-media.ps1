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

# Media Foundation's decoders on this machine (Windows Server has Media
# Foundation only as a feature): informational.
$out = & "$Dir/media_probe.exe" mf 2>&1 | Out-String
Write-Host "INFO  media: Media Foundation: $((($out -split "`n") | Where-Object { $_ -match '^mf: ' } | ForEach-Object { $_.Substring(4).Trim() }) -join '; ')"

# --- The framework: mediaapp.apk (tests/apps/mediaapp) ---------------------------------
if (Test-Path "$Dir/mediaapp.apk") {
  $bcpMedia = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
  # Gesher's default sounds, where $ANDROID_ROOT keeps them (the installer's
  # gesher-app.ps1 does the same): the settings point the default ringtones at them.
  if (Test-Path "$Dir/media/audio") {
    New-Item -ItemType Directory -Force (Join-Path $root 'system/media') | Out-Null
    Copy-Item -Recurse -Force "$Dir/media/audio" (Join-Path $root 'system/media')
  }
  $appCapture = Join-Path $mediaTmp 'app'
  New-Item -ItemType Directory -Force $appCapture | Out-Null
  # mediaapp's http check: tone_http.ogg from its APK, served on 127.0.0.1:8765
  # by a listener in another process for the run.
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path "$Dir/mediaapp.apk"))
  $httpFile = Join-Path $mediaTmp 'tone_http.ogg'
  $entry = $zip.GetEntry('assets/tone_http.ogg')
  if ($null -ne $entry) { [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $httpFile, $true) }
  $zip.Dispose()
  $httpServer = Start-Job -ArgumentList $httpFile -ScriptBlock {
    param($file)
    $bytes = [IO.File]::ReadAllBytes($file)
    $l = [Net.HttpListener]::new()
    $l.Prefixes.Add('http://127.0.0.1:8765/')
    $l.Start()
    while ($true) {
      $c = $l.GetContext()
      $c.Response.ContentType = 'audio/ogg'
      $c.Response.ContentLength64 = $bytes.Length
      $c.Response.OutputStream.Write($bytes, 0, $bytes.Length)
      $c.Response.Close()
    }
  }
  $env:GESHER_AUDIO_CAPTURE = $appCapture
  $mediaOut = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpMedia" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' com.gesher.app.Main (NoDrive "$Dir/mediaapp.apk") 2>&1 | Out-String
  $mediaExit = $LASTEXITCODE
  Remove-Item Env:GESHER_AUDIO_CAPTURE
  Stop-Job $httpServer; Remove-Job $httpServer -Force
  $appWav = Get-ChildItem $appCapture -Filter 'audio-*.wav' | Sort-Object LastWriteTime | Select-Object -Last 1
  Check 'media: AudioTrack in mediaapp.apk: streaming (short, byte, direct ByteBuffer writes) and static tracks, 8-bit/16-bit/float, mono and stereo, a marker, periodic notifications, loop points, setPlaybackRate, getTimestamp, AudioTrack.Builder''s defaults' {
    $global:LASTEXITCODE = $mediaExit
    $mediaOut
  } @('MediaTest audiotrack output: native rate 48000, min buffer (44100 mono 16-bit) ok, property rate 48000, frames per buffer ok',
      'MediaTest audiotrack 1 streaming 16-bit mono 44100: state 1, stream 3, wrote 22050, position 22050/22050, timestamp true, session ok',
      'MediaTest audiotrack 2 streaming 8-bit mono 8000: wrote 3200, position 3200/3200',
      'MediaTest audiotrack 3 static float stereo 48000: states 2->1, wrote 38400, marker true at 9600, position 19200/19200',
      'MediaTest audiotrack 4 bytebuffer 16-bit stereo 22050 volume 0.5: wrote 35280, position 8820/8820, periodic notifications 4, underruns ok',
      'MediaTest audiotrack 5 legacy static loop: stream 3, wrote 12800, setLoopPoints 0',
      'MediaTest audiotrack 6 playback rate: set 0, rate 48000, wrote 14400, position 14400/14400',
      'MediaTest audiotrack 7 builder defaults: rate 48000, buffer at least the minimum true, wrote 14400 in time true, position 14400/14400',
      'MediaTest done', 'gesher-app: done')
  Check 'media: AudioManager in mediaapp.apk: stream volumes (aliases, set, adjust, mute), the ringer mode, getDevices, audio focus (a transient loss, the gain back, a loss), isMusicActive' {
    $global:LASTEXITCODE = $mediaExit
    $mediaOut
  } @('MediaTest audiomanager volumes: music 15/15 min 0, ring 7/7, system follows ring true',
      'MediaTest audiomanager set music 7: 7, raise 8, mute true 0, unmute 8, back to 15',
      'MediaTest audiomanager ringer silent: mode 0, ring volume 0, notification muted true',
      'MediaTest audiomanager ringer normal: mode 2, notification volume 7/7',
      'MediaTest audiomanager devices: 1 output(s) [type 2, sink true, name Gesher audio capture, 48000 Hz true, stereo true], 0 input(s)',
      'MediaTest audiomanager focus: granted 111, changes [A-2, A1, A-1]',
      'MediaTest audiomanager music active: before false, playing true, after false')
  Check 'media: MediaPlayer in mediaapp.apk: MP3, AAC (M4A and ADTS), Ogg Vorbis, Opus, FLAC, WAV (16 and 24-bit, mu-law) from raw resources, assets, a path and http; prepare and prepareAsync, completion, pause, seekTo, looping, errors' {
    $global:LASTEXITCODE = $mediaExit
    $mediaOut
  } @('MediaTest mediaplayer mp3 (create, raw): duration ok, playing true, completed true, at end ok',
      'MediaTest mediaplayer aac (m4a, asset): prepared true, duration ok, playing true, completed true, at end ok',
      'MediaTest mediaplayer vorbis (ogg, raw): prepared true, duration ok, playing true, completed true, at end ok',
      'MediaTest mediaplayer opus (asset): prepared true, duration ok, playing true, completed true, at end ok',
      'MediaTest mediaplayer flac (asset): prepared true, duration ok, playing true, completed true, at end ok',
      'MediaTest mediaplayer wav (file path): prepared true, duration ok, playing true, completed true, at end ok',
      'MediaTest mediaplayer aac (adts, asset): prepared true, duration ok, playing true, completed true, at end ok',
      'MediaTest mediaplayer g711 mu-law (wav, asset): prepared true, duration ok, playing true, completed true, at end ok',
      'MediaTest mediaplayer seek (24-bit wav): duration 2000, paused at ok, stays true, playing false, seek complete true at 1500, completed true',
      'MediaTest mediaplayer looping: looping true, playing after 1.2 s true, completed after setLooping(false) true',
      'MediaTest mediaplayer http (ogg): prepared true, duration ok, playing true, completed true, at end ok',
      'MediaTest mediaplayer errors: start before setDataSource [-38/0], junk file IOException')
  Check 'media: MediaMetadataRetriever and SoundPool in mediaapp.apk: tags, MIME types and durations of an MP3, an M4A and a FLAC; two sounds loaded and played, one at twice its rate' {
    $global:LASTEXITCODE = $mediaExit
    $mediaOut
  } @('MediaTest retriever mp3: title Gesher Tone, artist Gesher, album Media Tests, mime audio/mpeg, audio yes, duration ok',
      'MediaTest retriever m4a: title Gesher Tone, artist Gesher, album Media Tests, mime audio/mp4, audio yes, duration ok',
      'MediaTest retriever flac: title Gesher Tone, artist Gesher, album Media Tests, mime audio/flac, audio yes, duration ok',
      'MediaTest soundpool: loaded true [1:0, 2:0], streams true true')
  Check 'media: Ringtone and RingtoneManager in mediaapp.apk: the default ringtone, notification and alarm sounds are Gesher''s; the default notification plays' {
    $global:LASTEXITCODE = $mediaExit
    $mediaOut
  } @('MediaTest ringtone defaults: ringtone file:.../Gesher_Ring.ogg, notification file:.../Gesher_Notification.ogg, alarm file:.../Gesher_Alarm.ogg',
      'MediaTest ringtone notification: playing true, ended true')
  Check 'media: AAudio and OpenSL ES from an app''s NDK library (mediaapp.apk''s libmedia_ndk_test.so): a data callback stream, a blocking-write stream, a buffer queue player, a player of an asset by file descriptor; no input streams yet' {
    $global:LASTEXITCODE = $mediaExit
    $mediaOut
  } @('MediaTest ndk aaudio callback: rate 48000, channels 2, float 1, state AAUDIO_STREAM_STATE_STOPPED, burst ok, frames ok',
      'MediaTest ndk aaudio write: rate 22050, written 8820, read all',
      'MediaTest ndk aaudio input: AAUDIO_ERROR_UNAVAILABLE',
      'MediaTest ndk opensl queue: playing 1, buffers left 0, taken 12, callbacks all',
      'MediaTest ndk opensl fd: realize 0, duration ok, at end 1')
  if ($null -eq $appWav) {
    Write-Host "FAIL  media: mediaapp.apk's audio output was not captured"
    $failed++
  } else {
    CheckSounds 'media: mediaapp.apk''s sounds reach the output (AudioTrack, AudioManager volumes, MediaPlayer, SoundPool): each tone at its frequency, duration and level, at its stream''s volume (none while the ringer is silent)' $appWav.FullName @(
      @(440, 0.5, 0.354), @(500, 0.4, 0.354), @(660, 0.4, 0.354), @(880, 0.4, 0.177),
      @(1000, 0.6, 0.354), @(1200, 0.3, 0.354), @(750, 0.3, 0.354),
      # AudioManager: music at 7/15; the silent ringer's notification is not heard.
      @(700, 0.3, 0.165), @(1300, 0.3, 0.354),
      # MediaPlayer: MP3, M4A, Vorbis, Opus, FLAC, WAV, ADTS, mu-law; the seek; the loop; http.
      @(523, 0.8, 0.354), @(587, 0.8, 0.354), @(659, 0.8, 0.354), @(698, 0.8, 0.354),
      @(784, 0.8, 0.354), @(880, 0.8, 0.354), @(988, 0.8, 0.354), @(1047, 0.8, 0.354),
      @(330, 0.3, 0.354), @(330, 0.5, 0.354), @(880, 1.6, 0.354), @(1175, 0.8, 0.354),
      # SoundPool: the WAV, then the Vorbis tone at rate 2.
      @(880, 0.8, 0.354), @(1318, 0.4, 0.354),
      # Ringtone: the default notification sound.
      @(1319, 0.6),
      # AAudio (callback, write), OpenSL ES (buffer queue at -6 dB, file descriptor).
      @(1397, 0.5, 0.354), @(1480, 0.4, 0.354), @(1568, 0.6, 0.177), @(659, 0.8, 0.354))
  }
} else {
  Write-Host 'INFO  media: mediaapp.apk not in the build output; its checks skipped'
}

# --- Video (stage 2, ADR 0051): videoapp.apk (tests/apps/videoapp) ---------------------
# Clips of 2 s at 320x240, red, lime, blue and white for 0.5 s each: each decoded frame's
# color says which frame it is.
if (Test-Path "$Dir/videoapp.apk") {
  # What Windows composed of a window's client area (PrintWindow with
  # PW_RENDERFULLCONTENT: a GL swap chain's frames too), as 0xRRGGBB.
  if (-not ('GesherMediaCapture' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GesherMediaCapture {
  [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] struct BMIH { public int Size, W, H; public short Planes, Bits; public int Comp, SizeImage, X, Y, Used, Important; }
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr CreateDIBSection(IntPtr dc, ref BMIH bmi, uint usage, out IntPtr bits, IntPtr s, uint o);
  [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
  [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr o);
  [DllImport("gdi32.dll")] static extern bool GdiFlush();
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  // {width, height, pixels...} or null.
  public static int[] Capture(IntPtr hwnd) {
    RECT r;
    if (!GetClientRect(hwnd, out r)) return null;
    int w = r.R - r.L, h = r.B - r.T;
    if (w < 1 || h < 1) return null;
    IntPtr screen = GetDC(IntPtr.Zero), dc = CreateCompatibleDC(screen);
    var bmi = new BMIH { Size = 40, W = w, H = -h, Planes = 1, Bits = 32 };
    IntPtr bits;
    IntPtr bmp = CreateDIBSection(dc, ref bmi, 0, out bits, IntPtr.Zero, 0);
    int[] px = null;
    if (bmp != IntPtr.Zero) {
      IntPtr old = SelectObject(dc, bmp);
      if (PrintWindow(hwnd, dc, 3 /* PW_CLIENTONLY | PW_RENDERFULLCONTENT */)) {
        GdiFlush();
        px = new int[w * h + 2];
        px[0] = w; px[1] = h;
        Marshal.Copy(bits, px, 2, w * h);
        for (int i = 2; i < px.Length; i++) px[i] &= 0xffffff;
      }
      SelectObject(dc, old);
      DeleteObject(bmp);
    }
    DeleteDC(dc);
    ReleaseDC(IntPtr.Zero, screen);
    return px;
  }
}
'@
  }
  # Runs videoapp.apk in a window shown off the desktop; when it says its surfaces are
  # ready, reads the window back at the TextureView's and the SurfaceView's centers.
  function RunVideoApp([string] $tag, [string[]] $extra) {
    $bcpVideo = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
    $log = Join-Path $mediaTmp "videoapp-$tag.log"
    $env:GESHER_AUDIO_CAPTURE = Join-Path $mediaTmp "video-$tag"
    New-Item -ItemType Directory -Force $env:GESHER_AUDIO_CAPTURE | Out-Null
    # hwui's SkiaGL pipeline over ANGLE for the 'gl' run (docs/angle.md).
    if ($tag -eq 'gl') { $env:GESHER_HWUI_RENDERER = 'skiagl' }
    $p = Start-Process -FilePath "$Dir/dalvikvm.exe" -PassThru -NoNewWindow -RedirectStandardOutput $log `
      -RedirectStandardError "$log.err" -ArgumentList (@("-Xbootclasspath:$bcpVideo",
        "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xusejit:false', 'com.gesher.app.Main',
        (NoDrive "$Dir/videoapp.apk"), '--hidden-window') + $extra)
    $read = $null
    $closed = $false
    $deadline = (Get-Date).AddSeconds(300)
    while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
      Start-Sleep -Milliseconds 500
      $text = (Get-Content -Raw $log -ErrorAction SilentlyContinue) + (Get-Content -Raw "$log.err" -ErrorAction SilentlyContinue)
      $m = [regex]::Match($text, 'VideoTest surfaces ready: texture view center (\d+),(\d+), surface view center (\d+),(\d+)')
      $h = [regex]::Match($text, 'first frame drawn \(\d+ presented\), \w+, hwnd 0x([0-9a-f]+)')
      # The app's checks are done: closing its window ends the run (WM_CLOSE finishes it).
      if ($h.Success -and $text -match 'VideoTest done' -and -not $closed) {
        [void][GesherMediaCapture]::PostMessageW([IntPtr][Convert]::ToInt64($h.Groups[1].Value, 16), 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        $closed = $true
      }
      if ($m.Success -and $h.Success -and $null -eq $read) {
        Start-Sleep 1
        $px = [GesherMediaCapture]::Capture([IntPtr][Convert]::ToInt64($h.Groups[1].Value, 16))
        $read = 'no capture'
        if ($null -ne $px) {
          $at = { param($x, $y) '{0:x6}' -f $px[2 + [int]$y * $px[0] + [int]$x] }
          $read = "texture view #$(& $at $m.Groups[1].Value $m.Groups[2].Value), surface view #$(& $at $m.Groups[3].Value $m.Groups[4].Value), background #$(& $at 10 300)"
        }
      }
    }
    if (-not $p.HasExited) { $p.Kill($true); Write-Host "INFO  media: videoapp.apk ($tag) did not end within 300 s" }
    Remove-Item Env:GESHER_AUDIO_CAPTURE, Env:GESHER_HWUI_RENDERER -ErrorAction SilentlyContinue
    $text = (Get-Content -Raw $log -ErrorAction SilentlyContinue) + (Get-Content -Raw "$log.err" -ErrorAction SilentlyContinue)
    return @{ Out = $text; Exit = $(if ($p.HasExited) { $p.ExitCode } else { -1 }); Read = $read }
  }
  # A window color near the expected one (each channel within 40).
  function NearColor([string] $hex, [int] $want) {
    if ($hex -notmatch '^[0-9a-f]{6}$') { return $false }
    $c = [Convert]::ToInt32($hex, 16)
    foreach ($s in 16, 8, 0) { if ([math]::Abs((($c -shr $s) -band 255) - (($want -shr $s) -band 255)) -gt 40) { return $false } }
    return $true
  }
  function CheckSurfaceColors([string] $name, $run) {
    $m = [regex]::Match("$($run.Read)", 'texture view #([0-9a-f]{6}), surface view #([0-9a-f]{6})')
    if ($m.Success -and (NearColor $m.Groups[1].Value 0xff0000) -and (NearColor $m.Groups[2].Value 0x0000ff)) {
      Write-Host "PASS  $name (read back: $($run.Read))"
    } else {
      Write-Host "FAIL  $name (read back: $($run.Read))"
      Write-Host (($run.Out -split "`n") | Where-Object { $_ -match 'VideoTest|GesherMedia|Exception|FATAL' } | Select-Object -First 40 | Out-String)
      $script:failed++
    }
  }
  $video = RunVideoApp 'cpu' @()
  $videoOut = $video.Out
  $videoExit = $video.Exit
  if ($video.Exit -ne 0) {
    # Why the app ended badly: the runtime's fatal lines and the last of the app's own.
    Write-Host (($video.Out -split "`n") | Where-Object { $_ -match ' F dalvikvm|JNI DETECTED|VideoTest|GesherMedia.* (E|W) ' } | Select-Object -First 60 | Out-String)
  }
  Write-Host "INFO  media: $((($videoOut -split "`n") | Where-Object { $_ -match 'VideoTest codec list \(info\)|GesherMedia.*video decoders' } | ForEach-Object { $_.Trim() }) -join '; ')"
  Check 'media: MediaExtractor in videoapp.apk: an MP4''s H.264 and AAC tracks, csd-0/csd-1 as Annex B parameter sets, samples, sync samples and times' {
    $global:LASTEXITCODE = $videoExit
    $videoOut
  } @('VideoTest extractor h264.mp4: 2 tracks [video/avc 320x240, audio/mp4a-latm 44100 Hz 2 ch], duration ok, csd-0 SPS true, csd-1 true, 60 samples, 4 sync, Annex B true, last at 1966 ms')
  Check 'media: MediaCodecList in videoapp.apk: the decoders this machine has (H.264 through Media Foundation, the audio decoders), their capabilities; no encoders' {
    $global:LASTEXITCODE = $videoExit
    $videoOut
  } @('VideoTest codec list: avc decoder c2.gesher.avc.decoder, aac decoder c2.gesher.aac.decoder, avc encoder null, 1920x1080 true, flexible YUV true, profiles 5, software true')
  Check 'media: MediaCodec in videoapp.apk: H.264 decoded to I420 ByteBuffers and an Image (synchronous), with a callback (asynchronous), a seek with flush; every frame in order, each quarter''s color; the AAC track to PCM' {
    $global:LASTEXITCODE = $videoExit
    $videoOut
  } @('VideoTest codec h264 sync: c2.gesher.avc.decoder, 320x240 color 19, 60 frames, ordered true, colors [red, lime, blue, white], image 35 320x240 3 planes red',
      'VideoTest codec h264 async: finished true, format changes 1, 60 frames, ordered true, colors [red, lime, blue, white]',
      'VideoTest codec h264 seek: before 5 frames, extractor at 1000 ms, first frame after the flush at 1000 ms, blue',
      'VideoTest codec aac: 44100 Hz 2 ch, encoding 2, 2 s, 1661 Hz',
      'VideoTest done')
  Check 'media: MediaCodec into a TextureView''s SurfaceTexture and a SurfaceView''s surface in videoapp.apk; TextureView.getBitmap' {
    $global:LASTEXITCODE = $video.Exit
    $video.Out
  } @('VideoTest surfaces: texture view 8 frames rendered, getBitmap 320x240 red; surface view valid true, 38 frames rendered')
  CheckSurfaceColors 'media: the window shows the TextureView''s and the SurfaceView''s frames where they are (hwui''s CPU pipeline), the TextureView red, the SurfaceView blue' $video
  Check 'media: MediaMetadataRetriever.getFrameAtTime in videoapp.apk: the closest frame, the previous sync frame, a scaled frame' {
    $global:LASTEXITCODE = $video.Exit
    $video.Out
  } @('VideoTest retriever frames: video 320x240 yes, at 1.25 s closest 320x240 blue, at 1.6 s previous sync 320x240 white, scaled at 0.25 s 160x120 red')
  Check 'media: MediaPlayer with video in videoapp.apk: the clip''s H.264 into a TextureView with its AAC audio, the video size, the frames shown, completion, A/V sync (the frame shown matches the audio position within 120 ms); VideoView plays to its end' {
    $global:LASTEXITCODE = $video.Exit
    $video.Out
  } @('VideoTest player video: size 320x240 (listener 320x240), duration ok, frames shown 30+, completed true, at the end ok',
      'VideoTest player sync: ',
      'in sync',
      'VideoTest video view: prepared true, duration ok, playing true, completed true')
  $videoGl = RunVideoApp 'gl' @('--ez', 'surfacesOnly', 'true')
  Write-Host "INFO  media: SkiaGL run: $((($videoGl.Out -split "`n") | Where-Object { $_ -match 'VideoTest surfaces: ' } | Select-Object -First 1))"
  $glRenderer = (($videoGl.Out -split "`n") | Where-Object { $_ -match 'VideoTest surfaces renderer: ' } | Select-Object -First 1)
  if ("$glRenderer" -match 'skiagl') {
    CheckSurfaceColors 'media: the same with hwui''s SkiaGL pipeline (ANGLE): the TextureView red, the SurfaceView blue' $videoGl
  } else {
    Write-Host "INFO  media: SkiaGL run read back: $($videoGl.Read) ($("$glRenderer".Trim()))"
  }
  # The other codecs: Gesher's software decoders (compat/media/video/soft_video.cc) where
  # Media Foundation has none: VP8 and VP9 (libvpx) from WebM (AOSP's MatroskaExtractor),
  # AV1 (libdav1d) and H.265 (libhevc) from MP4.
  foreach ($c in @(
      @{ Name = 'vp8'; What = 'VP8 (libvpx) from a WebM clip (MatroskaExtractor)' },
      @{ Name = 'vp9'; What = 'VP9 (libvpx) from a WebM clip (MatroskaExtractor)' },
      @{ Name = 'av1'; What = 'AV1 (libdav1d) from an MP4 clip' },
      @{ Name = 'hevc'; What = 'H.265 (libhevc, B-frames reordered) from an MP4 clip' })) {
    $line = ($videoOut -split "`n") | Where-Object { $_ -match "VideoTest codec $($c.Name) sync: " } | Select-Object -First 1
    Write-Host "INFO  media: $("$line".Trim())"
    Check "media: MediaCodec in videoapp.apk: $($c.What) decoded to I420 ByteBuffers and an Image; every frame in order, each quarter's color" {
      $global:LASTEXITCODE = $videoExit
      $videoOut
    } @("VideoTest codec $($c.Name) sync: c2.gesher.$($c.Name).decoder, 320x240 color 19, 60 frames, ordered true, colors [red, lime, blue, white], image 35 320x240 3 planes red")
  }
} else {
  Write-Host 'INFO  media: videoapp.apk not in the build output; its checks skipped'
}
