# Native2 source compatibility and historical Scandir evidence

Native2 repairs ownership of fdopendir/dirfd and relative filesystem operations.
These are current production changes; the archived Scandir timings are not new
measurements of this source. No native runtime or performance approval follows
from this adapter (`source_compatibility_only=true`, `native_runtime_approval=false`).

The outer adapter pins the complete live Bionic source and one complete patch.
It reverses that patch to the immutable qualified endpoint
`3c7fe0dd2813cd6495e303405b974fc46dfea30f`, verifies exact bytes, and applies it
forward to reproduce the live source exactly. The Scandir/sorting routine must
remain byte-identical. All other report inputs pass through unchanged. This
projected source also enters selected/vector's historical source reader. The
projected report source then enters the existing consolidation adapter: its complete
directory-section equality, reversible transition to the original `3a309b7`
endpoint, original adapter, original archive verifier, and original negative
controls are preserved. The archive, observations and timing summaries remain
unchanged.

New negative controls reject changed live bytes, a refreshed current hash with
an unchanged patch, a stable-merge comparator mutation even after refreshing
its source/manifest hashes, altered baseline bytes even with a new manifest,
modified baseline identity/scope/approval declarations and malformed patches.
The final positive and negative proof must be rerun against the final reviewed
source snapshot after the Native2 implementation stabilizes.

The predecessor commit IDs describe provenance, not required objects in a future
checkout. Squash merging followed by branch deletion need not retain these
feature commits. Native2 reconstructs the predecessor by reversing its complete
pinned patch and requires the independent historical Bionic SHA256
`9ba77321ff4a4fedd64b9dfc433a5c93349daf6eef90552bda95763b0ef3a531`.
The AppContainer outer transition similarly retains all 44 earlier predecessor
hashes, adds only two broker diagnostic paths, and independently pins the complete
46-path predecessor digest map before reversing and replaying its patch. When
original Git objects are available, their bytes must also match exactly.

`feature-source-snapshots.zip` supplies only exact feature donor sources needed
by the existing identity/property and USB proofs: 31 identity donor files, two
property donor files, two property predecessor files, one `82bb674` USB baseline
file, and six effective `af2a394` USB baseline files. Its SHA256 is
`74fdf84140949bf70e17781ebfe98528b9c68898f44d13445dc4067cb682e94c`.
It has exactly 42 members, 264092 compressed bytes and 865257 source bytes.
The archive digest, exact member list, member and total size bounds, and each
original donor/source digest are verified before use. No source is executed from
the archive. The donor reverse/forward checks and all historical controls remain.
An isolated empty Git repository verifies these transitions without any feature
commit objects; corruption and even repinned missing, extra or modified members
are rejected. The selected-policy selftest runs these durability tests.

Existing historical dependencies already present on main are unchanged. The
original Scandir `3a309b7` and archival-verifier `d5c8e60` endpoints are ancestors
of main. This adapter creates no measurements, native runtime approval, or
waiver of the original directory-section equality or sorting checks.

Final source-only validation passed on the frozen 78-input snapshot
`43e13f19c17579e336b842195b0791744c130e1aac0466aff7a688a034940b6a`:
selected-policy `--selftest`, vector-policy, Scandir report (9496 checks,
728 archive members, 11 new Native2 controls and unchanged 40 consolidation plus
30 original controls), five archival-report unit tests, and three isolated
source-durability tests. All commands exited zero. The six original outer
transition controls remain and a seventh rejects a repinned predecessor map.
Original historical measurements and all non-draft report fields are unchanged.
These results are source compatibility evidence; actual native runtime and
visible desktop acceptance remain separate gates.
