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

MediaStoreContractTest passed 25 assertions in an isolated runtime with no
Binder server: collection/member URI paths; metadata/query constants; original
and pending URI flags preserving existing query and fragment; audio key
normalization; thumbnail size validation; selection/argument/default-order
forwarding through a wrapped resolver; and playlist item move URI/value/result.
Runtime logs are under `out/stability/java-values-candidate`.

The queries and bitmap/thumbnail operations delegate to ContentResolver exactly
as the upstream implementation does. This supplies client contracts; it does
not supply a MediaProvider, cloud photo backend, permission-prompt activity,
media index, scanner, transcoder or thumbnail generator. The prior local
provider-unavailable policies remain. Image decoding/insertion and actual
thumbnail generation require separate end-to-end provider/media tests and are
not proven by the forwarding regression.
