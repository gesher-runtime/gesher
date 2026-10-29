# Bounded DNS wire name decoding (test92, ADR0070)

Exclusive source branch wip/net-dns-wire starts at accepted
f1550540186b4ae3f17a5e501a886ec1c4d5d613. The six symbols ns_name_unpack,
ns_name_ntop, ns_name_uncompress, ns_name_skip, dn_expand and __dn_skipname
were absent from the actual bionic symbol registration; the inventory alone
did not prove their implementation. This slice supplies real System V ABI
bridges and byte-based DNS name decoding. Inventory proof/history is unchanged.

Primary semantic source is bionic android-16.0.0_r4
libc/dns/nameser/ns_name.c, SHA256
ff0e3bf977cd2c7a4ced96b6577ace4b92d82739ba39eaf42cdb4051358eb4f6.
The exact upstream file and ISC license are retained in tests/native2/dns-reference.
Its selected functions are compiled and executed independently in CI, alongside
the system Linux libresolv implementation, against the same literal wire fixtures.
No native compilation or fixture execution is permitted locally.

The production decoder checks compression targets, bounded pointer traversal,
message/output lengths, invalid labels, root conventions, escaped binary octets
and the pinned obsolete bitstring label type. ns_name_skip deliberately requires
a complete name or compression pointer: the pinned implementation accepts an
empty/truncated label ending exactly at eom; that historical malformed-input bug
is not replicated. The cursor remains untouched on failure. No read occurs beyond
the supplied packet end for an extended label header. ns_name_ntop, like bionic,
requires a complete uncompressed input name because its API supplies no input size.

The new component rebuilds the complete production host and matching translator,
both NDK ELF ABIs, and the independent Linux/AOSP oracle on the current source and
attempt. Strict source inventory, compiler identities, typed member digests,
actual artifact ZIP receipts, PE/ELF roles and negative controls are required.
Both Windows runners compare actual child output to executed oracle output.

Status: production source bb101f6f4a056168fe5765ca054e54dde6862f43 passed
independent integration-worker source peer after correcting the pinned bitstring
headroom rule. Component40804487806 is active on that exact source; Root owns
the sole watch. The fixture requires 96 common Linux/AOSP records, 138 extended
AOSP records (including capacities0..12), 100 malformed-boundary/cursor records,
and actual corrupted-expectation negatives for each ABI on both Windows runners.
Only local Python/PowerShell AST, canonical source pins and diff checks ran.
No runtime, full-suite or inventory verification is claimed yet.
Packing/encoding, RR parsing, resolver queries/cache/transport, other remaining
Net4 entries, hardware and aggregate release closure remain open. The allocation
is test92 under existing ADR0070; a later full integration must register this
bounded contract without relaxing historical whole-suite/package/e2e guards.
