# Notification persistence integration: delegation and toast identities

## Inputs and scope

This reconciles [PR139](https://github.com/gesher-runtime/gesher/pull/139)
(`f1b4f6229a088d73ef082e14a6315c1c5ccbdd4c`, tree `eec4ef50534ea9940673a469a7fddd9a2c7c01b7`)
with [PR142](https://github.com/gesher-runtime/gesher/pull/142) and its stacked
[PR145](https://github.com/gesher-runtime/gesher/pull/145)
(`652667832c69a6bf9e39af5ae9c3800cf11f76fc`, tree `8bc9e884c1a8e09d0f804c9fc827b29212de6c6c`).
Frozen inputs and external branches remain unchanged. Both inputs use baseline
`473beec761bd3005a23a8f746a6ba9216343f39d`; the later main update is not included.

Both writers independently used version 4. After channelId, delegation wrote
opPkg; toast migration wrote windowsTag and legacyWindowsTag. This integration
changes the private record format and combines cancellation, preserving Android
keys, authenticated callers, grants/revocation, owner permissions and channels.
The original contract references and ordinary-APK observations remain in
[the F32 verification](issue56-f32-notification-delegation.md). Toast history
ownership is described in [TOAST_MIGRATION.md](../../tests/notification-identity/TOAST_MIGRATION.md).
Prior-branch Windows results do not qualify the combined tree.

## Unambiguous format and historical policy

New files start with integer5 and the String16 schema marker
`gesher.notification.poster-and-toast.v1`, followed by:

| Group | Ordered fields |
| --- | --- |
| Android identity | key, packageName, packageIdentity, tag, id, flags, channelId |
| Attribution and cleanup | opPkg, windowsTag, legacyWindowsTag |
| Activation state | content Intended, action count, action Intended records |

Intended presence is strictly zero/one. Present values contain a supported sender
kind, creator package/identity/UID, request code, flags, bounded Intent count,
framework Intent payloads and a nullable resolved-type array. Non-null type-array
length must match the Intent count, as the existing PendingIntents producer requires.

| Historical layout | Accepted poster | Accepted Windows identity |
| --- | --- | --- |
| v1 | Owner, existing legacy identity mode only | Original legacy tag |
| v2 | Owner with existing code identity checks | Original legacy tag |
| v3 | Owner, including retained channel | Original legacy tag |
| PR139 delegation v4 | Explicit opPkg | Original legacy tag |
| PR145 toast v4 | Owner; this writer predates delegation | Verified digest and optional legacy alias |
| Combined v5 | Explicit opPkg | Verified digest and optional legacy alias |

Both v4 tails are independently parsed from the same verified common header.
Each must validate all fields and consume the complete record. Exactly one valid
interpretation is required; both or neither means unreadable. The first string's
spelling never chooses attribution. Missing/empty poster data never defaults to owner.

A toast-v4 alias is null or a verified legacy tag. Null and multi-character aliases
cannot be Intended presence markers. A single-character alias is a decimal digit:
native String16 encodes a character outside the five supported sender kinds, so
the alternative reader fails before Intent deserialization. All ten digit aliases
with non-null content are covered. Hybrid v4, concatenated records, trailing data,
and any double-valid interpretation are rejected.

Scalar reads require four bytes; string lengths, decoded lengths, consumed
extents, bounded counts, Intent extents and EOF are checked. Unknown versions/
markers and malformed presence, kinds, counts or truncations fail closed.
Rejected files remain untouched. Existing full-key, package and protected code
identity checks precede tail acceptance.

Actual file length remains authoritative. [AOSP JNI unmarshalling](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/jni/android_os_Parcel.cpp) uses
`setDataSize(length)` then `writeInplace(length)`; the native write pads to a word
and advances dataSize in [native Parcel](https://android.googlesource.com/platform/frameworks/native/+/refs/tags/android-16.0.0_r4/libs/binder/Parcel.cpp). A missing trailing zero byte could otherwise reappear as
padding. The reader therefore requires `Parcel.dataSize() == data.length`
immediately after unmarshalling, before parsing. The first native-Parcel probe
exposed this boundary: its truncation assertion failed. That failed payload and
receipts are retained; the corrected probe has a distinct immutable payload.

Reads do not eagerly rewrite data. Reposting or successfully retiring a recorded
alias writes v5. Alias retirement preserves opPkg and Intended fields. Delegated
reposting retains the genuine old Windows tag for checked cleanup. Verified
legacy-filename fallback and same-key deletion remain; an unreadable current
file does not fall back through older state.

## Cancellation and retained fixes

Caller/owner resolution and original-poster checks run under the publication
lock before removal. A delegate cannot cancel another poster's retained record.
Toast-only and predelegation records remain owner-originated; a concurrent
replacement cannot change attribution between checking and removing.

Authorized cancellation then uses PR145's current/legacy removal path. A legacy
alias is only a cleanup hint: actual toast-history XML must match the full launch
key before removal. Colliding owners survive. Repost cleanup keeps aliases after
history failure or ownership mismatch. Hashed files/images, embedded-NUL and
HSTRING checks, and null/literal-`null` key equivalence remain.

Native identity/removal sources, WindowsHost, NotificationIcons and
NotificationIdentity are unchanged from PR145. NotificationDelegates and the
original grant/revocation, caller, permission and channel policy remain from
PR139. Original regression assertions are retained. External PR71's later native
and WindowsHost changes are a future integration dependency, outside this repair.

## Verification boundaries and required Windows gates

The complete combined production Java tree compiles against real framework/core
interfaces. Original 49 identity/toast and 88 channel/group checks remain.
The format corpus covers all layouts, Unicode/unpaired surrogates, every sender
kind with real framework Intent/action bodies, single-digit aliases, malformed
counts, truncation through Intent/type data, exact EOF, read-only rejection,
restart and alias retirement.

The host runner uses an explicit Parcel seam, including native-style alignment
and unmarshalling padding. Its private encoding/null-Bundle-only support is not
Android wire qualification. Delegation with `--with-storage` exercises original
assertions plus historical v4 cancellation in protected and legacy profiles.
Denied cancellation never reaches removal; allowed cancellation preserves a
colliding Windows-history owner.

The reference builder freezes unchanged production Store/Identity and explicit
non-Android service dependencies. It generates request JSON and never runs ADB.
Its corrected v2 full-corpus capsule was built and preserved without execution.
The subsequent `--normal-only` v3 capsule contains a separate valid-format corpus;
it excludes the malformed corpus and old entrypoint entirely. Only
com.gesher.server definitions are packaged. It asserts native boot providers for
Parcel/Intent/Notification, API36/ART, integer representation and Unicode.
Separate write/read VMs qualify valid v4/v5 record bytes, actual v5 rewrites and
persistence. Malformed-input rejection and the raw-length invariant are reviewed
against primary source and the bounded host seam, without another malformed
native device run. Service identities are explicit fixtures; installation,
signer verification, Binder identity and APK permissions are outside this probe.
PendingIntent creation/activation is forbidden.

The root-owned API36 execution of the frozen normal capsule passed 97 write
checks and 16 read checks in separate processes (receipts 052/053). Both used
native boot Parcel providers under UID 2000. Payload SHA256:
`c22a163920312fc4aeae18abf3a9ffb318139da3b9ff543a4a7469f694413ed9`.
This qualifies ordinary valid-format/Unicode/migration compatibility only.
The detailed source/payload/receipt binding is retained in
[`native-format-reference.json`](../../tests/notification-identity/reference/native-format-reference.json).

| Combined-tree validation | Result and boundary |
| --- | --- |
| Java production compilation | 281 runtime inputs plus 46 client SDK fixture inputs; all 327 compile against real framework/core interfaces |
| Services fixture build | 153 inputs compile and produce API36 DEX |
| Identity/toast and format host corpus | 49 original checks plus 307 format checks; explicit host Parcel encoding |
| Delegation with retained storage | 166 protected and 155 legacy checks; constructed package/process/native boundaries |
| Channel/group regression | 88 original checks |
| Stock-Android valid formats | 97 write + 16 read; native Parcel, owned files, explicit non-Android service dependencies |
| Native cross-compilation | Three actual x64 translation units and linked COM fixture; no Windows execution |
| Workflow/runner syntax | YAML, Python and track94 PowerShell parsed |

The native compile database came from an N28-adoption environment. Its modified
winpthreads include overlay conflicts with the frozen 473 baseline; that initial
configuration failure is retained. Reusing the pinned compiler's stock baseline
headers compiles all three units without source changes or warning suppression.
No managed SDK/header was changed, and no full combined Windows runtime was linked.

Windows track94 runs the built production storage and native Parcel in separate
write/read VMs with a fresh fixture data directory. The existing delegation gate
keeps generated Binder Parcel policy coverage. The standalone Windows ART tool
also runs the format corpus; native toast-history fixtures are unchanged.

Actual Windows execution of this combined tree remains a merge gate. No canceled
workflow was rerun or Windows CI manually dispatched. Host, cross-compile and
stock-Android evidence do not establish Windows toast, installed-APK or full F32
qualification. Final evidence binds source hashes, payload/receipt, exact tree
and independent review before publication.
