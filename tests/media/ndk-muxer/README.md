# NDK muxer fixture preparation

Source only; no build or runtime acceptance yet. Run generation and every
native/JVM/C# compilation or fixture only in CI. Test79 is reserved.

generate-fixtures.py makes a two-second baseline H.264/AAC clip without
B frames and extracts actual packets from it and the existing VP9/Opus and
VP8/Vorbis clips. ffprobe provides encoded bytes, codec configuration and
presentation timestamps independently of Gesher's writer. Per-track times
are checked for monotonic order; a single recorded shift makes the earliest
packet nonnegative. JSON audits preserve source and packet hashes, exact
field types, byte lengths and times. No packet is synthesized as a stand-in.

The little-endian binary layout is eight bytes GSMUX001, uint32 track
count, then each track's uint32 field count. Each field contains a uint32
key length plus UTF-8 key, uint32 kind (1=int32, 3=string, 4=bytes), uint32
value length and bytes. After tracks come uint32 packet count and each
packet's uint32 track, int64 timestamp in microseconds, uint32 flags,
uint32 payload size and actual encoded payload.

Source for real typed NDK JNI/ELF/APK callers now includes both ABIs,
offset/ownership/lifecycle/error/FD-access controls and both interleaved and
track-blocked packet arrival. It is uncompiled/unexecuted preparation.
Independent container metadata/packet/decoded color/tone checks and the
test79 runner are still required before component CI. The native bridge's ten entries
and MP4 location source remain unverified. Append, HEIF, B-frame/DTS/CTS
and advanced metadata contracts remain open and must not be reported as
covered by these presentation-ordered fixtures.
