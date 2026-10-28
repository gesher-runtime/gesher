# Source preparation for the next Media4 NDK extractor slice

Preparation only, outside every frozen candidate. No implementation, new
worktree/test reservation, compiler or executable fixture was started.
The current muxer full40527788805 remains frozen and independent.

Owner-requested nearest-checkpoint stop: this plan and its source receipt
are saved as documentation only; do not infer implementation or CI from it.
Saved exact android-16.0.0_r4 primary sources and URL/SHA256 receipts are in
media4-extractor-source-audit.json. The NDK wrapper source SHA256 is
512bf90bc9119a4ba4e5175aedf441bff5f86bc61768900ab7d8eaa4e0eccd5a;
NuMediaExtractor.cpp is
cee78caa5ce58987739ce4684f5c8747a4e3f1418f153a444f9ad03abf8266ff.
The browser could not open Gitiles; four small base64 text sources were
retrieved directly into the review directory and decoded without execution.

The existing real backend is compat/media/player/extractor.{h,cc}, with
the pinned AOSP extractor plugins, real AMediaFormat ownership, FD/memory/
path/HTTP data sources and per-track readers. Existing Java extractor
state in compat/media/jni/media_extractor_jni.cc already reads ahead one
sample per selected track and chooses the earliest timestamp. Reusing that
pure state requires a deliberate factor; copying the JNI implementation
would create competing selection/seek/error behavior.

Candidate first scope is a real ordinary-file selection/read/seek lifecycle
over those plugins: new/delete, setDataSourceFd, count/format, select/
unselect, sample read/time/index/flags/size, advance and seek. This is an
implementation proposal, not a promise of all twenty-one public entries.
The exact public wrapper and NuMediaExtractor contracts must decide each
error/state ordering before code, rather than Java convenience behavior.

Source-driven acceptance requirements:

- Duplicate and retain the caller FD and honor a nonzero offset/length
  range; close the original before actual sample reads. Independently
  verify caller offsets, bounds, invalid/nonseekable descriptors and a
  second setDataSource operation against the pinned state ordering.
- Owned track/file formats must outlive wrapper calls and extractor
  deletion, with exact pinned conversion of codec-specific data and track
  IDs. Raw plugin csd-avc/csd-hevc storage alone does not prove the NDK
  format contract; the Java helper already performs real conversions.
- Duplicate selection is idempotent; real unselect/reselect behavior and
  current-sample tie ordering must match NuMediaExtractor. Keep no-selected
  tracks, end-of-stream and reader failures distinguishable internally.
- Read the same current sample repeatedly without advancing; a too-small
  destination must follow the pinned capacity contract without sentinel
  corruption or silently moving the cursor. Compare actual encoded packet
  bytes/hashes, track indexes, timestamps, sync flags and final EOF.
- Seek all selected tracks using the real reader option; verify previous,
  next and closest sync independently. The NDK wrapper maps other enum
  values to next-sync, unlike a naive static cast.
- Use both native ABIs through core JNI, standalone ELF and real
  single-ABI APKs in single/device modes. Keep original Java/media tests.
  Source headers transitively include the unresolved LLP64 Codec off_t/
  long assertion; any private declaration must carry exact host and real
  NDK caller ABI assertions without claiming the full header adaptation.

Known backend seams requiring work, not assumed completion:

- AospTrackReader currently stores its failed start internally while
  OpenTrack can still return a non-null reader; selection must not report
  success for an unusable reader. Read errors are currently collapsed to
  a generic negative result.
- Sample retains data/time/sync/Vorbis valid-samples only. PSSH, encrypted
  subsamples/keys/IVs, SEI, MPEG user data, audio presentations, temporal
  layers and sample offsets are not available through that struct.
  getSampleCryptoInfo, getPsshInfo and full getSampleFormat must remain
  open until real metadata and ownership exist; generic empty/null exports
  are not an implementation.
- Custom AMediaDataSource callbacks need real SysV/ARM callback lifetime,
  read/size/error and close ownership tests. They are not supplied by a
  Java path wrapper or by an always-failing callback export.
- HTTP cache duration is a real duration/bitrate/buffering contract. The
  pinned file case returns-1, but registering a global constant-1 would
  falsely cover network behavior. Native URI/HTTP source policy also
  requires its own pinned service/capability review.

Next action after the current components: request the coordinator's unique
test reservation, use a separate component branch/worktree from accepted
integration, then prepare a small real factor/typed caller/oracle and
compile-first CI. No native inventory or roadmap entry changes follow from
this source plan.
