# 0059: Recording: a capture engine, encoders, container writers, MediaRecorder

Status: accepted (media3 track, stage 3; docs/media.md "Recording")

## Context
Apps record sound through AudioRecord (or AAudio and OpenSL ES input in native
code) and MediaRecorder, encode through MediaCodec's encoders (raw buffers, or
an input Surface they or a camera draw into) and write files with MediaMuxer.
On a phone these reach AudioFlinger's record threads, Codec2's software and
hardware encoders and libstagefright's MPEG4Writer/WebmWriter, all in other
processes. A Windows PC may have a microphone or not; CI runners have none
(and no audio endpoint at all), but must still verify what is recorded.

## Decision
- **A capture engine per app process** (compat/media/audio/capture.h), the
  counterpart of the output engine: one thread takes the input source's audio
  every 10 ms and hands each started stream its share, converted to the
  stream's rate (linear interpolation), channels and PCM format. The source
  is GESHER_AUDIO_SOURCE's WAV file when set (played as the microphone, paced
  by the clock, from its start each time recording starts from idle, looping:
  what CI feeds), GESHER_AUDIO_SOURCE=loopback (WASAPI loopback of the
  default output), else the default capture endpoint in WASAPI shared mode
  (the PC's microphone), else silence paced by the clock (recorders still
  open, as a muted microphone does; AudioManager lists no input device).
  AudioRecord, AAudio input streams, OpenSL ES audio recorders and
  MediaRecorder are streams of it.
- **The record permission** is the manifest's RECORD_AUDIO (Gesher grants what
  a manifest asks for, as it does CAMERA): without it AudioRecord's setup
  fails (the record stays uninitialized, startRecording throws), AAudio input
  answers AAUDIO_ERROR_UNAVAILABLE, OpenSL ES SL_RESULT_PERMISSION_DENIED,
  MediaRecorder.setAudioSource throws, as AudioFlinger and the audio policy
  refuse on a phone.
- **Encoders** (compat/media/encode) behind MediaCodec's existing slots and
  callback model: H.264 through Media Foundation's H.264 encoder MFT (in system
  memory, low-latency mode: a frame out per frame in; I420 in, Annex B out),
  AAC-LC through Media Foundation's AAC encoder MFT, Opus through libopus (the
  AOSP build already had its encoder). What comes out is what Android's
  encoders give: the codec-config buffer first (the SPS and PPS, the
  AudioSpecificConfig, the OpusHead), access units flagged as key frames,
  the output format's csd-0/csd-1 (Opus: csd-1/csd-2, the delay and pre-roll).
  libavc's encoder is built for a fallback where Media Foundation lacks
  H.264; MediaCodecList lists only what works on the machine.
- **Input surfaces** (createInputSurface, MediaRecorder.getSurface) are
  QueueSurfaces (the surfaces of stage 2) whose frames, RGBA, are converted to
  I420 for the encoder: Canvas (lockCanvas), ANativeWindow_lock and the camera
  service's frames (CameraFrames draws into any native window) all work. A
  camera records through Camera2 with the recorder's Surface as a session
  output (VIDEO_SOURCE_SURFACE); the legacy Camera's recording
  (VIDEO_SOURCE_CAMERA with setCamera) is not provided.
- **Local Camera2 callbacks** go through CameraCallbacks' serial executor,
  preserving ICameraDeviceCallbacks' one-way ordering and argument lifetimes.
  CameraDeviceImpl holds its interface lock while calling disconnect, and
  its callbacks need that same lock: synchronously calling a callback from
  the capture thread while disconnect joins it creates a lock cycle. Only
  the capture thread owns/closes the native capture handle; disconnect never
  joins the callback executor. Clients keep their callback executor alive
  until onClosed, then shut it down. This fixes terminal ordering separately
  from the still-open device camera throughput gate in docs/media.md.
- **Container writers** of Gesher's own (compat/media/mux): MPEG-4/3GPP (the
  samples in one mdat as they come, the moov at the end: avcC, esds, dOps,
  stts/stss/stsz/co64, an edit list where a track starts late), WebM through
  libwebm's mkvmuxer (frames interleaved by time), Ogg Opus and AAC ADTS.
  MediaMuxer is the first two; MediaRecorder all four.
- **MediaRecorder** is Gesher's recorder (compat/media/record) in place of
  StagefrightRecorder: the capture stream and the input surface feed encoder
  threads that write the container; getMaxAmplitude, pause/resume, the
  maximum duration and size (MEDIA_RECORDER_INFO_*).
- **MediaCodec into an ImageReader**: a decoder configured with an
  ImageReader's Surface hands each released frame to the reader (the devices
  track's in-process image queue, through com.gesher.server.CameraFrames), as
  YUV_420_888 Images.

## Consequences
The Just Player e2e run (ci 40292991000) also exposed missing session-service
bootstrap. Gesher now installs MediaFrameworkPlatformInitializer's manager
and serves media_session over Binder. Session owners publish metadata,
playback state, a queue and extras; controllers query them and forward
transport commands to the owner's callback. Owner death removes its session.
The controller round trip and all 30 media3 checks pass on both runners
and in both modes in ci 40318189310; Just Player's e2e launch also passes
on both runners. The run still has two older output-measurement failures
on windows-2022, so it is not a fully green integration result. Windows
system transport controls, media-key receiver fallback, Session2/media
communication, listener permission/user filtering and external volume-change
notifications remain open work (docs/media.md and the explicit acceptance
checks in docs/handoff/tracks/media4.md).

The capture abstraction shares the recorder path between WASAPI and CI's
file source. CI makes the input known and checkable end to end (recorded,
encoded, muxed, read back, decoded, measured); it does not establish physical
microphone, loopback, route-change or endpoint-removal behavior. No AMR, Vorbis or VP8/VP9/HEVC
encoders: MediaRecorder falls back to AAC (3GPP/MPEG-4) or Opus (WebM/Ogg)
for audio and refuses other video encoders; persistent input surfaces, the
legacy camera's recording, hardware (DXVA/D3D11) encoding and B-frames are
not provided. Audio effects on input (AEC, AGC, noise suppression) are not
applied.
