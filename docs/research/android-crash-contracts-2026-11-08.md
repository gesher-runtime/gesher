# Android compatibility and crash-contract audit, 2026-11-08

The inspected upstream baseline is
`a4dd7242cfed1a08cd8761ae896d63faf7b8cf63` (after PR #7). Existing
November 5 fixes were checked before selecting additional work. The audit
covers current Java service/value implementations, package parsing and
queries, native allocation/OS adapters, ELF failure paths and the existing
compatibility inventories. It does not establish that every possible APK
runs, or that every Android API behaves correctly.

Android's [CTS overview](https://source.android.com/docs/compatibility/cts)
separates signature checks from behavioral tests. This distinction matters
here: an existing class can lack an overload, a service can inherit a null
Default answer, and a resolved native function can still corrupt memory.
The reference release for this repair is the project's pinned Android 16 r4,
rather than an unrelated newer platform API.

## Confirmed findings repaired

| Area | Failure in the inspected source | Repair and regression evidence |
| --- | --- | --- |
| Native aligned allocation | `size + alignment` could overflow and return storage smaller than requested. A failed aligned `realloc` freed the original block, leaving an ordinary recovery path with freed memory. | Check arithmetic and retain storage/ownership until replacement allocation succeeds. Production-source ASan/UBSan regressions and shared ELF tests cover overflow, contents, retries and ownership. |
| Remaining allocator contracts | `reallocarray` was not exported; zero-size aligned `realloc` and zero-dimension `calloc` differed from bionic; `posix_memalign` failure changed errno. | Add checked `reallocarray`, correct zero-size behavior, and preserve errno/output on `posix_memalign` failure. See the [allocator report](native-allocation-contracts-2026-11-08.md). |
| Native group queries and file locks | `getgroups` could write through a null output pointer and accepted negative sizes. `lockf` returned success without acquiring or releasing a range. | Validate group arguments. Route nonblocking lock/unlock/test to the existing fcntl record-lock layer. Blocking `F_LOCK` explicitly returns `ENOSYS`; it does not claim to acquire a lock. See the [system-adapter report](native-system-contracts-2026-11-08.md). |
| Socket control data | `sendmsg` silently discarded nonempty ancillary data while sending the payload; batched sends could likewise claim successful control delivery. | Reject unsupported control messages before transmission, with `EOPNOTSUPP`. Real socket peers verify that no forbidden payload is sent and that batch counts stop at the failed message. This does not implement descriptor transport. See the [socket report](native-socket-control-contracts-2026-11-08.md). |
| DNS-SD manager APIs | Three public request overloads were absent; stop callbacks used the wrong executor, listener equality could conflate registrations, and rejected submissions could leave unusable state. | Restore the descriptors and repair request ownership/executor handling. The actual production classes pass controlled-executor tests; the old class fails descriptor lookup. See the [NSD report](nsd-contracts-2026-11-08.md). |
| Audio queries | Actual framework wrappers dereferenced null communication-device and head-tracking arrays. Other public non-null lists also inherited null Defaults. | Return consistent empty/unavailable capability values and test the real ContextImpl managers. See the [audio report](audio-query-contracts-2026-11-08.md). |
| Subscription queries | Generated defaults reported the valid identifier `0` despite an empty subscription inventory, inviting initialization paths that assume a SIM exists. | Return the pinned framework's absent subscription/slot identifiers, preserve its distinct unmapped-phone sentinel, and provide non-null empty ID arrays. The host fixture passes 26 production-service assertions; public manager wrappers join the Windows fixture. This is a reproduced contract defect, not a reproduced third-party APK crash. See the [subscription report](subscription-contracts-2026-11-08.md). |
| Package metadata | `getPackageInfoVersioned` returned null for installed ordinary packages, canonical-name mappings returned null arrays, and parsed manifest properties were discarded by queries. | Delegate ordinary package lookup to existing metadata, preserve name-array positions, and expose the parsed application/component property values. Test the actual parser, generated AIDL proxy/stub, Parcel transport and ApplicationPackageManager, including unknown-user/package and removal cases. |
| Installed split APKs | Installation copied splits but parsed only the base manifest and synthesized names from filenames. Feature components, dependency graphs and class-loader policy were missing. | Parse validated installer-owned clusters through AOSP, preserve the isolated loading flag, and use the full graph during install/reload, shortcuts and dexopt context selection. Reject malformed staging before replacing the running installation. Real feature DEX/resource fixtures cover dependencies and failed-update preservation in Windows CI. See the [split-package report](package-split-contracts-2026-11-08.md). |
| Proxy initialization | The documented nullable copy constructor dereferenced null. A valid PAC description with its initial local port of -1 threw while converting to a Java proxy. | Accept the empty snapshot and return the framework's nonthrowing conversion result while a local port is unavailable. Reject null PAC URIs as Android does. Host value tests and Windows Parcel tests cover pending, ready and absent proxies. |

The proxy reference is
[pinned AOSP ProxyInfo.java](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework/src/android/net/ProxyInfo.java).
The implementation retains its existing normalized exclusion-list representation
and local parcel encoding; these checks do not establish AOSP wire-format
interoperability or complete direct-host validation.

The package references are pinned framework
[ApplicationPackageManager](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/app/ApplicationPackageManager.java),
[ComputerEngine](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/ComputerEngine.java),
and [PackageProperty](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/PackageProperty.java).
A VersionedPackage version selects static shared-library providers on AOSP;
it does not require the version of an ordinary installed app to match.
The patch therefore does not invent a new mismatch error for ordinary apps.

## Validation inputs and boundaries

The Java compile inputs come from successful repository CI run
[42425141954](https://github.com/gesher-runtime/gesher/actions/runs/42425141954),
source `a6749ab7655398a51ebd719d6d9b129d87e2888e`. The framework inputs
(`aosp.lock`, patches, framework/generator/host build sources and core-module
construction) were compared with the inspected baseline and are unchanged.

| Input | SHA-256 |
| --- | --- |
| GitHub artifact 13595267948, framework/core compile inputs | `e27b851b2cd2ac52481c341ea102e6f959a10dc528373e7cec5a35ad6668ed79` |
| Inner framework classes ZIP | `8d8976799d323066c360c174f60c83a8ce08604c46d4864b0c25e70000a46020` |
| Inner core module ZIP | `ec67a47be8680d60687f03e80c2e50e386f2f4a1298cd8de3289adebedfebb49` |
| GitHub framework artifact 13595730967 | `56341de876dc1b9111068823d3b7b62c9021886b8a05388f9b3e801a273d9adb` |
| Reference framework-res.apk | `1e79a1235f26794e1c0d0bf6c186bee90cd8a98a57f8e47c5144c954e3d14a3f` |

The source combination passes Java compilation and D8 generation for the app
and service tests. The local app compilation uses pinned core/java sources
for compat-table generation and does not assemble all optional AOSP source
modules. Its output is a compile-only check, not a distributable or installed
runtime. The normal CI build regenerates its full inputs from the source tree.

The NSD host fixture passes 37 assertions, the proxy host slice passes 20,
and the subscription host fixture passes 26.
They execute the production classes against the actual framework/core compile
inputs, without replacement Android API stubs. The audio report distinguishes
actual wrapper failures from the supplemental host diagnostic that bypasses
Windows initialization. The complete package fixtures, successful communication/
subscription manager paths and native Parcel checks require the Windows ART runtime.
The split-package builder also passed aapt2, javac and D8 compilation; its
binary manifests were inspected. Building those APKs is not equivalent to
executing the installed feature class-loader/resource assertions.

The native allocator, OS-adapter and socket tests pass 35, 41 and 40 assertions
respectively (116 total). They compile the corresponding production sections
and exported registrations verbatim, with an explicitly described
host envelope. Their shared fixtures also join the existing native ELF suite
for Windows x86_64 and translated ARM64. Local ASan/UBSan runs disable only
LeakSanitizer because this environment cannot inspect the required process
state. They are not Windows execution, cross-ABI proof or a leak-check result.
The lightweight CI gate runs all three sanitizer harnesses before the larger build.

## Follow-up of the open PR

A second pass starts from PR #8 head
`3843241968edfd124d3b9596c0e6381f45f5c381`. It traces public framework callers,
actual service state, installer generations, native exports and data transfer
through the implemented backends. Successful empty queries are counted as
correct only where the runtime's actual inventory is empty. Explicit refusal
of an unsupported operation is not counted as implementation of that feature.

| Area | Additional confirmed defect | Implemented follow-up |
| --- | --- | --- |
| Directory APIs | Public `scandirat`, `scandirat64`, `telldir` and `seekdir` exports were absent; scanning could dereference failed allocations or return a partial list after a read error. | Actual descriptor-relative scans and stream-position restoration; checked allocation/cleanup and real enumeration-error propagation. [Native follow-up](native-followup-2026-11-08.md). |
| Synchronization | Semaphore counts could exceed Android's maximum; distant absolute deadlines overflowed signed nanoseconds. | Atomic bounded posting and wide deadline arithmetic, including concurrent posters and actual waits. [Native follow-up](native-followup-2026-11-08.md). |
| Audio transfers | Valid oversized direct-record requests were rejected; array arithmetic could overflow; extra playback vectors could abort across JNI on allocation failure. | Capacity-clamped capture, checked JNI buffer ranges, VM-managed playback array access, preserved payload/copy-back behavior, and wide byte counts until sample conversion. [Audio buffer report](audio-buffer-contracts-2026-11-08.md). |
| Installed package behavior | Derived manifest policy flags were lost; the base-only cache hid changed splits; full installs inherited omitted optional splits; required split/type validation was absent. | Project all parsed flags, execute real changed-cluster validation/replacement, distinguish raw-base reuse from full replacement, and preserve the active generation on invalid required sets. [Package follow-up](package-followup-2026-11-08.md). |
| Package queries/completion | Existing packages appeared absent to group queries; bulk identities/permission holders inherited defaults; deprecated dex-module requests never completed. | Queries read actual package/permission state. Dex registration follows Android 16's deliberately disabled operation and its asynchronous failure callback; no dex-registration backend is claimed. [Package follow-up](package-followup-2026-11-08.md). |
| Storage/user state | Storage inventory and user bundles/names were null; unknown serials mapped to the owner; CE-lock state contradicted the unlocked session; allocation requests could claim success without checking capacity. | Real filesystem-backed volume/path/state/capacity queries and existing owner-user metadata; measured capacity checks with actual settings/reserves and permissions. [Service follow-up](java-services-followup-2026-11-08.md). |
| Notification channels and groups | Group APIs inherited defaults, channel policy disappeared on restart, missing modern channels were fabricated, and deletion did not coordinate active notifications or running services/jobs. | Persistent channel/group state, real queries and membership, policy-preserving updates, manager-cache invalidation, blocked-channel enforcement and coordinated removal of live/retained notifications. [Notification follow-up](notification-groups-followup-2026-11-08.md). |
| Job notifications | `setNotification` ignored the end policy and shared ownership; completing jobs could leave notifications and UIJ flags behind. | Validate and track actual run associations, preserve shared notifications, apply the final association's REMOVE/DETACH policy, and update retained state when the last UIJ flag is removed. [Job lifecycle report](job-notification-followup-2026-11-08.md). |
| Native file growth | Invalid/overflowing ranges and read-only descriptors could report successful allocation; POSIX errno behavior was incorrect. | Checked ranges, actual handle rights, descriptor lifetime and error propagation around existing CRT file growth. Sparse/range physical reservation and concurrent-resize parity remain explicitly unimplemented. [File allocation report](file-allocation-contracts-2026-11-08.md). |

The added native directory, semaphore/deadline and file-operation harnesses
pass 72, 76 and 40 assertions. Together with the rerun 116 original native
assertions, that is **304 native host assertions**, with the same ASan/UBSan
and LeakSanitizer boundary described above. The separate real-JVM JNI test
passes **72 audio-buffer assertions** under checked JNI and UBSan.

New Java host evidence includes **75 manifest-flag**, **22 package identity/
callback**, **45 storage** and **49 user-manager** assertions against actual
production/framework classes. Their focused reports describe constructor and
native-boundary envelopes rather than treating those probes as ART runs.
The notification host fixture passes **88 assertions**, including actual manager
queries, durable XML reload, failed saves, blocked-post removal, user policy,
cache invalidation and publication races. The original PR head fails at the
public create-group/get-group operation. Its native initialization envelope is
explicit; host success is not Binder, retained Parcel-store or Windows UI proof.
The split builder now creates **nine real APK fixtures**, including revised,
required and type-providing splits; all pass build and alignment checks. The
signed required-base fixture also passes signature verification. Its real
PackageInstaller session checks the public incompatible-install status and
preservation of the active generation; execution of that fixture requires ART.

The complete combined application and service-test sources pass Java compilation
and D8 generation, including the real AIDL notification-group and job-lifecycle
fixtures now registered in the Windows suite. These remain compile-only local
results; the host probes do not execute the Binder transport or Windows ART.

The five changed POSIX/bionic translation units also compile as complete
Windows x86-64 and native Windows ARM64 objects, and both changed media JNI
units compile as Windows x86-64 objects. These checks use the exact CI
llvm-mingw release `20261006`, archive SHA-256
`5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4`.
The source preservation gate remains exact outside the reviewed file-allocation
function and pins that complete changed file independently. Cross-compilation
does not establish linking, Windows execution, or translated guest execution.

## Further implementations while CI runs

Work continued from published commit
`79567dadaacefd15983e9230bfd80290d6cd52f9` while its runtime gates ran:

- **Activity options:** `queryIntentActivityOptions` now resolves actual installed
  components, supplies specific indices and manifest labels/icons, applies
  supported query flags and Android's combination/deduplication order. The
  maintained host fixture passes **107 checks**, with a separate 600-request
  differential check of the pinned combination algorithm. Chooser/preference
  and broader visibility-policy limits remain explicit in the
  [activity-options report](activity-options-followup-2026-11-08.md).
- **Wake-lock listeners:** Acquisition, replacement, release and token death
  now notify the actual current listener asynchronously. Death registration and
  retirement are tied to the held record; failed native requests can be retried.
  The production-service host fixture passes **98 assertions**; the real public
  manager/Binder fixture is registered for Windows. See the
  [power report](power-wake-lock-followup-2026-11-08.md).
- **Semaphore limit queries:** The exported `sysconf` now reports the same
  Android maximum enforced by `sem_init`/`sem_post`, from one shared constant.
  Three additional production-function checks bring that harness to **76**,
  already included in the native total above. See the
  [sysconf report](semaphore-sysconf-followup-2026-11-08.md).

The full application and service-test sources compiled and dexed together again
after these implementations. The first published follow-up's CI run
[42444051868](https://github.com/gesher-runtime/gesher/actions/runs/42444051868)
completed its builds, but its failed runtime/package consumers stopped during
GitHub job-inventory requests with HTTP 502/504 or HTTP/2 stream cancellation.
They did not reach the new runtime fixtures. The shared GET transport now makes
bounded retries for those exact transient failures while retaining every receipt
identity and digest predicate. **15 transport/acceptance tests** and the existing
**5 source-identity tests** pass; see the
[CI transport report](github-read-transport-followup-2026-11-08.md). New-revision
runtime execution is still required and cannot be inferred from this repair.

## Final lifecycle and Windows integration follow-up

Inspection continued from published source
`6c0e1976456e6214763c7bddf58229eb3d4f3f66` while its Windows tests ran.

| Area | Confirmed failure | Implemented behavior and evidence |
| --- | --- | --- |
| Audio focus | A rejected new death registration could destroy the previous focus owner. Reacquisition retained obsolete callback/lifetime state, late death notifications could remove a replacement, and duck-to-transient escalation and regain values were wrong. | Register before mutation; retain or replace complete records according to pinned gain/flag rules; retire exact generations and shared dead tokens; apply real loss/restore transitions; deliver ordered callbacks outside the lock. **206 production/framework host assertions** pass, and the old source fails on lost ownership after a rejected request. [Audio focus report](audio-focus-followup-2026-11-08.md). |
| Explicit URI grants | Direct provider/path permission could not be delegated; exact URI identity discarded query/fragment; parsed grant patterns were lost; unrelated grant identities could combine modes; recipients could not relinquish their own grants. | Use actual provider and permission state, preserve exact URI identity, enforce parsed grant patterns and per-identity modes, merge only identical grants, revoke the authorized recipients/modes, and fence package generations. **138 host assertions** pass, with six behavioral negative controls. Constructed AOSP package models are not an APK-parser or provider-file-I/O test. [URI grant report](uri-grants-followup-2026-11-08.md). |
| Cross-drive Windows paths | Both main Windows jobs failed real Unicode ShellLink creation: a Java directory on C: was converted to a different path on D:, the runtime's drive. | Match the native `/mnt/<drive>` mapping before separator conversion and preserve ordinary-path boundaries. **23 production Java assertions** pass; the old source reproduces the exact wrong-drive destination. The mandatory Windows shortcut test retains real creation, existence and deletion checks. [Path report](zygote-path-followup-2026-11-08.md). |

These bring the follow-up Java host total to **851 assertions**
(`484 + 206 + 138 + 23`). This is a count of executed assertions, not an API
coverage percentage or proof of arbitrary-APK compatibility. The Windows
fixtures additionally exercise the actual generated audio/AM Binder parcels
and public audio manager; local host probes do not establish those paths.

Run [42446512495](https://github.com/gesher-runtime/gesher/actions/runs/42446512495)
passed the artifact transport/identity steps that previously failed, the full
framework/application/native builds, packaging and the dedicated Binder/web
regressions. Its two main Windows jobs then stopped at the real shortcut
assertion above, before the new framework-contract track. That failure is not
reported as a transient network error or a completed service-test pass. The
final revision requires its own Windows run after the path repair.

## Earlier Windows contract feedback

The additional `framework-contracts` matrix leg runs on both Windows versions
using the same current-source artifact verification and assembly as the normal
test leg. It retains the runner's initial VM/framework/lifecycle checks, then
runs the existing graphics and font prerequisites (`30-graphics`, `32-fonts`),
the actual service suite (`50-services`) and framework contracts
(`94-framework-fixes`). Every selected script is required to exist, and its
original assertions and exit handling remain active. The normal full Windows
jobs and their offline identity/retirement checks continue unchanged.

The selected leg provides earlier runtime feedback for these contracts. It does
not replace the complete suite or establish coverage of the omitted tracks.

## Repairs driven by actual Windows execution

The early framework-contract jobs on both Windows versions, source
`8af8d702bef18a7a6caead597595542b3aa41183`, run
[42450333175](https://github.com/gesher-runtime/gesher/actions/runs/42450333175),
exposed seven failures before the longer full suite finished. These were traced
to their actual runtime or fixture causes; none is accepted as a passing test.

| Observed failure | Repair and required evidence |
| --- | --- |
| Reloading real notification policy produced `Unexpected magic 1032ffff`. | The common `pread`/`pwrite` implementation now uses an independently opened file object, preserves the shared sequential cursor, enforces original access rights, and retains handles/buffers through asynchronous completion. The raw binary XML sniffer, service restart, unlinked files, delete-on-close files and concurrent aliases are all mandatory Windows assertions. See the [positioned-I/O report](positioned-io-followup-2026-11-08.md). |
| Four package/activity/split/URI fixtures could not create a temporary directory because the boot runtime selected nonexistent `/tmp`. Production XAPK installation and APK icon extraction use the same API. | Initialize the actual Windows temporary path before ART, expose it as libcore's changeable `java.io.tmpdir` default, retain explicit VM/environment overrides and the application's later cache selection. Real file operations determine existence/access failures. A stable UNC namespace avoids Java collapsing a network path's double slash. Ten fresh-VM scenarios require real create/write/read/delete and failure behavior, with a separate actual-SMB file/temp check. See the [temporary-directory report](temp-directory-followup-2026-11-08.md) and [UNC report](unc-path-followup-2026-11-08.md). |
| The foreground-service assertion expected a deleted diagnostic message. | The APK now queries the actual stored channel and verifies its ID, name and importance. The harness retains the real posted-toast assertion and also requires the actual cancellation/removal event. No production log is invented to satisfy the test. |
| The Wi-Fi fixture completed 42 checks successfully, while its runner still expected 33. | Require the current exact 42-check success marker and the original zero exit status. The fixture's behavior and assertions are unchanged. |

The full Windows suites additionally exposed two independent test defects:

- **Optimized-away allocator calls.** The actual `native-libs` artifact
  `13605117526`, source `8af8d7`, was downloaded and its archive digest verified
  (`sha256:94c37acc2d6d130502bf3e07102ab3418fbfb3623157da5df5e5dd00b7dfbd86`).
  Disassembly of `libc_test.so` shows the compiler removing allocation calls and
  producing unconditional failure branches or removing `errno` observations.
  For example, the overflow `calloc` check at `0x7444` stores zero in `errno`
  then prints failure without a `calloc` call. The shared regression now invokes
  volatile function-pointer slots initialized from the actual public imports.
  All original assertions and control flow remain; library optimization stays
  enabled. The production allocation implementation is unchanged. Its existing
  **35 host assertions** still pass under ASan/UBSan; independent `-O2` IR
  inspection retains all five import slots and the real indirect calls.
- **Different TCP/UDP port availability.** Full Windows 2022 job
  [128635404241](https://github.com/gesher-runtime/gesher/actions/runs/42448689136/job/128635404241)
  failed while a .NET UDP fixture tried to bind the ephemeral port already
  selected for TCP. The two real listeners now each ask Windows for an available
  port, and the Java fixture receives the corresponding ports independently.
  The same 50,000-byte TCP and UDP payload checks remain. Both listeners are
  cleaned up even if setup fails. A real local Java/socket run verifies the new
  argument routing; Windows execution remains required.

Before this repair batch, the existing public AudioManager focus, wake-lock
listener, job-notification lifecycle, package manifest/query helper, storage and
user-profile fixtures already passed on both early Windows jobs. Package APK
integration, split installation, URI Binder checks and persisted channel reload
were blocked or failed as described above and are not included in that claim.
The two full E2E jobs on source `8af8d7` also completed successfully; their launch,
frame/input and cleanup scope is separate from API completeness.

This section records the pre-publication checkpoint. New runtime assertions
require the repaired revision's own CI, with the original full suites and
source/artifact receipt checks still enforced. Historical independently pinned
component certificates are not rewritten to make the new source appear old.

## Additional native contract follow-up

Before closing the round, a further native pass against published `223d4b3b`
found two additional behavioral differences. Android's permissive `memalign`
rounds a non-power alignment upward; Gesher reduced it to 16, which could return
under-aligned storage. The separate C11 `aligned_alloc` export also incorrectly
used that permissive entry. Both now use their actual Android argument contracts
and the existing real allocation/free backend. **79 production-shim assertions**
pass under ASan/UBSan; the exact published source fails the two new groups while
passing the original five. See the [allocation report](native-allocation-contracts-2026-11-08.md).

The positioned-I/O follow-up also preserves Linux's unused-buffer and error
ordering: an empty-file or null-device read need not write a destination,
directories report `EISDIR`, and zero/null device writes can discard input while
random-device writes require it. These are checks on actual file/device state,
not a blanket acceptance of bad pointers. The production-slice host harness now
passes **110 assertions**, and the independent Linux kernel reference passes
**65**; the exact `223d4b3b` slice fails 11 new checks. Actual Windows NUL-device
classification is still a mandatory runtime check. See the
[positioned-I/O report](positioned-io-followup-2026-11-08.md).

The small descriptor-registry lookup required by that repair is the only new
`fd.cc` code. The active TCP preservation gate pins the complete reviewed file
and independently requires removing that exact wrapper to reproduce every byte
of its prior baseline. Missing, modified, duplicated and unrelated-source
mutations were all rejected without changing repository files. Historical
certificates, TCP behavior, supplier identity and receipt gates remain intact.
Main CI preflight now also runs the positioned-I/O sanitizer harness, the
100-check temporary-directory entry fixture and the 82-check UNC mapping
fixture. They supplement the mandatory Windows file/API checks.

Both focused Windows jobs for `223d4b3b` then reached the unchanged framework
`Os.memfd_create` assertion and failed with `ENOSYS`. The real storage backend
existed, but the common and x86-64 guest raw-syscall routes were absent. Both
now reach that backend. Its bounded repair validates names and flags, preserves
actual descriptor inheritance, requests executable mapping access, and cleans
up a created temporary file after a failed open. **193 production-source host
assertions** pass under ASan/UBSan; the exact published negative control fails
57 of its 121 executed assertions. Ten independent Linux checks and strict
complete-source Windows x86-64/ARM64 compilation also pass. Enforced seals,
huge pages and a no-execute seal remain absent; those requests are explicitly
rejected. See the [memfd report](memfd-syscall-followup-2026-11-08.md).

The active TCP gate pins the complete reviewed `system.cc` and independently
compares every byte outside the allocation and memfd function bodies against
its baseline. Five additional nonmutating negative controls reject altered
function bodies, unrelated `fcntl` behavior, an extra include and a changed
suffix. The preflight also runs the memfd sanitizer harness. These changes do
not relax supplier or artifact identity checks.

## Actual Windows checkpoint after the native and temporary-path repair

Run [42453120565](https://github.com/gesher-runtime/gesher/actions/runs/42453120565),
source `223d4b3b6a4947bd3c1b3c679d95fc944b4d956a`, confirms both Windows versions
pass the real notification-policy binary reload, ten local temporary-directory
VM scenarios, the 21 UNC file/NIO assertions, explicit URI Binder contracts,
public audio-focus callbacks and job-notification lifecycle. The two E2E jobs
also pass. These observations do not turn either focused job into a success:
each still reports exactly five failures at this checkpoint.

One is the real missing memfd route above. Three package/activity/split fixture
failures were blocked before their intended assertions by missing process
bootstrap: the actual system context needs a display service, and package
caches require writable native `ApplicationSharedMemory`. Their fixture setup
now provides the actual production display Binder and native shared memory,
retaining the real manager, generated AIDL/Parcel and cache-invalidation paths.
No cache is disabled. The remaining UNC temporary-default failure came from
PowerShell retaining an empty `TMPDIR` when asked to set it to null; the native
runtime correctly preserved that explicit override. The helper now removes an
absent variable through the environment provider and restores absent versus
empty state exactly. The new Windows runs must verify all five repairs.

The framework track now uses the existing bounded process-and-reader cleanup
helper; it no longer waits indefinitely on a redirected stream inherited by a
descendant after the primary VM exits. Exit codes, time limits and required
success markers remain enforced.

## Managed Activity caller identity and historical URI permissions

Managed activities now retain a stable initial caller token and separate
immutable caller records for each delivered Intent and result. The record
captures the authenticated package generation, UID, identity-sharing choice
and actual read/write permissions at delivery. It collects data, single or
multiple `EXTRA_STREAM` values, and nested ClipData Intents. Current URI grants
and historical caller access remain distinct, so revocation cannot rewrite an
old caller's snapshot. Queries validate the querying app's actual provider
access before returning the historical result.

The same records pass through real `ReferrerIntent`, `ResultInfo` and launch
transactions, including activity recreation. PendingIntent attribution uses
the verified creator and the normalized creator/sender sharing options. A
request's accepted lifetime retains the creator capability and code generation;
the holder being finished by that request's own `CLEAR_TOP` cannot silently
cancel the queued callback. Target generations and authenticated publication
are still checked.

The production controller/policy host fixture passes **116 assertions**, with
constructed installed/process state and explicit native boundary adapters.
The prior controller fails the write-permission negative control. Separate
components and peer APKs compile through javac, D8, aapt2 and zipalign and are
mandatory in both focused and full Windows runs. They exercise actual initial,
new-Intent, recreation, result and cross-package PendingIntent callbacks, plus
real provider descriptor contents. Their Windows execution is pending at this
checkpoint; host assertions are not represented as cross-process ART proof.
See the [caller lifecycle report](component-caller-followup-2026-11-08.md) for
the primary Android references, exact evidence and remaining boundaries.

## Material compatibility work still open

These current-source limits are separate from the repaired defects:

- **Service and Java module coverage.** The source inventory still finds many
  inherited Binder Default methods. These are review candidates, not a count
  of crashes: some are optional, some are handled elsewhere, and some need
  valid callbacks or object values. Bluetooth's local manager has no adapter
  and its API surface is partial. An absent hardware feature does not ensure
  that every SDK callback/value class required while loading an app exists.
- **Real discovery and audio policy.** The NSD implementation has no mDNS
  backend. Communication-route selection, spatialization and Android recording
  inventories remain absent. Empty results prevent the identified wrapper
  crashes; they do not implement those capabilities.
- **Native readiness and descriptor transport.** `compat/posix/src/epoll.cc`
  still explicitly treats `EPOLLET` as level-triggered. Socket ancillary-data
  transport, including SCM_RIGHTS, is not implemented. Blocking record-lock
  waits remain a separate backend obligation.
- **Storage backends.** OBB image mounting, per-UID cache accounting/quotas,
  safe live-app cache deletion and ordered free-space reclamation remain absent.
  Native sparse/range preallocation and concurrent file-resize guarantees are
  also incomplete. Returning an error, an empty quota, or logical file growth
  is not presented as implementation of those operations.
- **Broader package policy.** Android's preferred-activity/chooser backend,
  full caller/target compatibility policy, multiuser package state and complete
  package visibility remain outside the implemented owner-user model. The new
  activity-options implementation selects a real candidate for ambiguous
  implicit specifics rather than claiming an implemented system chooser.
- **Further activity and URI policy.** The managed-activity caller records do
  not implement automatic Intent/ClipData grants, persistable permissions,
  persisted task restoration, full task-affinity selection or dynamically
  delegated provider authorization. The standalone `GesherAppDriver` does not
  own a managed ActivityRecord and still lacks that caller lifecycle. Existing
  explicit grants and per-delivery snapshots do not substitute for these
  separate backends.
- **Further descriptor policy.** General file-status flag changes, enforced
  file seals and random-device entropy mixing remain incomplete. A supported
  memfd backed by a real temporary file does not establish Linux shmem
  accounting, all mount-policy independence or unsupported sealing behavior.
- **Remaining notification/job policy.** Old retained notification records from
  store versions 1 and 2 have no channel ID, so selective channel deletion cannot
  identify them after restart. Distinct user-stop/background-restriction job
  reasons, full UIJ scheduling/quota policy and broader Android notification
  policy remain outside the implemented normal association lifecycle.
- **Native exports and ABI behavior.** The platform soname table is conditional
  on implemented probe symbols. A named library or successful symbol lookup
  does not establish every export, layout, callback or ownership contract.
  `compat/elf/elf_loader.cc` still aborts on invocation of an unresolved native
  function; required missing imports can therefore still terminate an app.
- **Integration beyond these fixtures.** Full ARM64 instruction/signal/JIT
  behavior, media/graphics backends, physical devices and wider application
  lifecycle combinations require their own evidence. Existing implemented
  slices are preserved; older audit counts are not current failure totals.

The existing [central map](systematic-compatibility-2026-11-05.md) and
[inventory guide](../compatibility-audit.md) provide the broader navigation.
The Java/NDK/service/JNI inventories measure different overlapping surfaces;
their numbers must not be added or turned into an Android-support percentage.
