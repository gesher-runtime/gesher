# MediaStore public contracts restored

The SDK member inventory identified missing constants and nested collection
contracts on MediaStore. The source now retains the actual Android 16 r4
definitions for the absent intent/query constants, metadata columns, Audio
albums/artists/genres/playlists collections and members, download/picker columns,
image query/bitmap/insertion helpers, and image/video thumbnail forwarding and
cancellation. Original/pending URI flags, the pinned picker limit (100), legacy
Audio.keyFor and the Downloads row-ID URI helper are also restored. Public
legacy constructors match the SDK; the thumbnail classes remain extensible.

The exact upstream URL, upstream/local SHA-256 and excerpt adaptations are in
[the source receipt](aosp-mediastore-contracts-2026-11-05.json). The upstream
Apache-2.0 copyright/license header is retained in the implementation.

MediaStoreContractTest passed 28 assertions in an isolated runtime with no
Binder server: collection/member URI paths; metadata/query constants; original
and pending URI flags preserving existing query and fragment; audio key
normalization; thumbnail size validation; selection/argument/default-order
forwarding through a wrapped resolver; playlist item move URI/value/result;
and unavailable-provider insertion/cleanup and null input streams after provider
death. The final helper returns null when
the provider fails to insert a row and when a write and its cleanup both fail.
Bitmap decoding also closes its input stream on exceptions. These are limited
failure-handling adaptations to the pinned implementation.

The earlier implementation was run with the same final resolver fixture as a
negative control: it exits 1 with a NullPointerException from deleting a null
URI after insertion returned null. The candidate exits 0. All four final value
fixtures pass 127 assertions in private interpreter ART processes. Runtime
logs and exact input/source hashes are under
`out/stability-worktree/out/java-review/qa-receipt.json`; no installed files or
shared app processes were changed.

The queries and bitmap/thumbnail operations delegate to ContentResolver, with
the failure handling described above. This supplies client contracts; it does
not supply a MediaProvider, cloud photo backend, permission-prompt activity,
media index, scanner, transcoder or thumbnail generator. The prior local
provider-unavailable policies remain. Image decoding/insertion and actual
thumbnail generation require separate end-to-end provider/media tests and are
not proven by the forwarding regression.
