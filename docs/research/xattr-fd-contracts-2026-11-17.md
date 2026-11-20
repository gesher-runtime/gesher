# N42: current authorization and Bionic FD contracts

This is a focused follow-up to persistent user attributes in PR116, based on
`7eeed9b1c0fdc7e9d25a0d6bc7daebb71a28e698`. It preserves that implementation's
`GESHER.USER.XATTR` / `GXA1` native EA representation, bounded decoder, safe guest
memory copies, descriptor pinning, file-identity update mutex and remote-volume
exclusion. It does not close N42 or introduce security/trusted/system namespaces.

## Target contracts and sources

Android's VFS checks current inode read permission for a user-attribute read and
write permission for mutation. An earlier successful open does not preserve the
old authorization for later xattr calls. Listing is a separate operation: denial
of file-data read access does not by itself prohibit listing attribute names.
See the [pinned Android kernel xattr implementation](https://android.googlesource.com/kernel/common/+/964a54e5e1a0d70cd80bd5a0885a1938463625b1/fs/xattr.c).

Bionic's public `fgetxattr`, `fsetxattr` and `flistxattr` first attempt their raw
syscall. On `EBADF`, an actual `O_PATH` descriptor triggers a corresponding path
operation through `/proc/self/fd`. Public `fremovexattr` has no such fallback.
The legacy raw fd syscalls therefore differ from three public libc functions.
The selected Android16 sources are
[fgetxattr.cpp](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/fgetxattr.cpp),
[fsetxattr.cpp](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/fsetxattr.cpp), and
[flistxattr.cpp](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/flistxattr.cpp).

| Provider on a valid O_PATH regular-file fd | Get | Set | List | Remove |
| --- | --- | --- | --- | --- |
| Android Bionic public functions | Attribute value | Mutation | Attribute names | EBADF |
| Legacy raw fd syscalls | EBADF | EBADF | EBADF | EBADF |
| Linux/glibc public functions used by the host oracle | EBADF | EBADF | EBADF | EBADF |

## Windows implementation

Windows has distinct [file-data and EA access rights](https://learn.microsoft.com/en-us/windows/win32/fileio/file-security-and-access-rights).
Eligible file operations now request `FILE_READ_DATA` for get, `FILE_WRITE_DATA`
for set/remove, and the EA rights needed by the persistent map. List requests EA
read rights without requiring data read. A nofollow regular-file get receives the
same authorization as a following get, after the held object has been classified.
Symlink classification precedes mutation authorization, preserving nofollow errors.

Path and fd operations reopen the pinned object with a native empty name to
perform the filesystem's current-token check. This neither resolves a replacement
pathname nor enables backup-intent privilege overrides. Windows
[file-open rules](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/8ada5fbe-db4e-49fd-aef6-20d54b748e40)
can reject a delete-pending object even when an existing handle remains usable.
Only that specific native failure permits retaining an already admitted handle,
and only if it already carries all native EA rights plus `READ_CONTROL`.
That fallback queries the current security descriptor by handle and checks the
effective thread token (or the process token when no impersonation token exists).
It never treats an old grant as current authorization.

The fallback uses [GetSecurityInfo](https://learn.microsoft.com/en-us/windows/win32/api/aclapi/nf-aclapi-getsecurityinfo)
and [AccessCheck](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-accesscheck).
It also checks the token and file's actual mandatory integrity information so
DACL evaluation alone cannot permit a lower-integrity caller to mutate an
admitted file; see the [mandatory integrity algorithm](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-dtyp/ae69a089-473d-4c23-bf3d-7a12a9d11123).
No Windows labels are exported as invented Android security attributes.

### Delete-pending policy boundary (independent QA correction)

A plain DACL grant is insufficient when a file also carries a Central Access
Policy. The fallback now requests `SCOPE_SECURITY_INFORMATION` and
`ATTRIBUTE_SECURITY_INFORMATION` along with the owner, group, DACL and mandatory
label. Microsoft documents both additional fields as `READ_CONTROL` queries;
see [SECURITY_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/secauthz/security-information).
The snapshot also requests the SDK's process-trust-label and access-filter
metadata. [QuerySecurityAccessMask](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-querysecurityaccessmask)
must report exactly `READ_CONTROL` for the complete selection on the running
Windows version. A query error or unsupported selection fails the operation;
policy fields are never silently removed from the request.

Before evaluating a delete-pending descriptor, a bounded production ACL parser
admits only ordinary allow/deny DACL ACEs and the mandatory label. Applicable
resource-property, CAP identifier, callback/conditional, object-specific and
unknown policy ACEs return `EOPNOTSUPP`. Malformed metadata returns `EIO`.
Known inherit-only ACEs do not apply to the current object; inherited ACEs still
apply. The normal native reopen path remains unchanged and lets Windows evaluate
all of its supported policies. This is a declared fallback limit, not an
implementation of enterprise central authorization.

Microsoft's [CAP identifier ACE](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-dtyp/aa0c0f62-4b4c-44f0-9718-c266a6accd9f)
selects policy provisioned on the evaluating machine, and
[conditional ACEs](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-dtyp/10dc22eb-788d-4343-b556-0b6969fe58ca)
carry expressions in callback ACE data. Neither can be treated as an ordinary
unconditional grant. The Windows fixture uses
[AddResourceAttributeAce](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-addresourceattributeace)
and [SetSecurityInfo](https://learn.microsoft.com/en-us/windows/win32/api/aclapi/nf-aclapi-setsecurityinfo)
to install and read back an actual file resource property. It also installs a
real conditional DACL through SDDL. For both policies, the fixture directly calls
the same production held-object security evaluator and requires `EOPNOTSUPP` for
get/set/list/remove access masks. This is direct evaluator coverage; it does not
claim that every public call to a delete-pending file enters that fallback.

The fixture separately queries
[FILE_STANDARD_INFO.DeletePending](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_standard_info),
records the actual `NtOpenFile` status from the production native-open primitive,
and verifies that any successful reopened handle identifies the original file.
The file and effective token remain unchanged between this authorization control
and the corresponding public call. If Windows authorizes the native reopen, the
public call must complete its real EA operation. If native reopening reports
`STATUS_DELETE_PENDING`, the public call must refuse the unsupported policy.
No test switch forces a native status or selects a more permissive authorization
path. Separate set/remove attributes prevent a failed assertion from destroying
the base value or invalidating later measurement. After removing unsupported
metadata, both the manual evaluator and ordinary EA operations must work again.
A separate [AddScopedPolicyIDAce](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-addscopedpolicyidace)
control constructs a CAP identifier ACE only in memory and passes it to the
production parser. It does not install a central policy or establish enterprise
CAP enforcement. All native policy controls require Windows execution.

The same production parser has **61 portable assertions** covering simple policy,
resource/CAP/callback metadata, inheritance applicability, malformed lengths and
SID bounds, plus **10,000 deterministic corrupt inputs**. These passed with
ASan/UBSan. The component gate runs and seals this separate parser result; the
intentional bad-pointer syscall fixture retains its distinct sanitizer limitation.


The Bionic public wrappers use three explicit POSIX-provider helpers that allow
O_PATH admission. Both x86-64 and ARM64 raw syscall tables select separate
`__fgetxattr`, `__fsetxattr` and `__flistxattr` shims. Raw fd operations and public
remove retain `EBADF`. The public fallback follows the held object itself,
including an O_PATH fd to a symlink, without reopening its mutable pathname.

## Validation and limits

The standalone fixture calls actual public functions and all twelve raw syscall
numbers. It tests unreadable input, readonly output, page-boundary strings,
O_PATH provider differences, O_PATH symlinks, rename and pathname replacement,
dup/dup2 reuse, closed descriptors, and normal/O_PATH fd lifetime after unlink.
The same source is included in the existing x86-64 and ARM64 guest fixture, so
full CI continues to test actual guest dispatch rather than merely helper names.

Observed on the reference environment on 2026-11-17:

- Linux/glibc: **68 checks, zero failures**, including a UBSan run. The existing
  complete filesystem reference also passed **172 checks**, followed by the new
  68 checks, with a real second filesystem at `/dev/shm`.
- Actual Android36 x86-64, uid2000 shell, kernel
  `6.6.66-android15-8-gb66429556fb8-ab13070261`: **75 checks, zero failures**.
  Device job `015-n42-followup-fd` exited zero. The executable SHA256 was
  `52a7729673a133d042305b0e8ecc016fa856b9df911a61c2eed03ecf9e15474d`.
- Fresh Windows x86-64 and ARM64 production POSIX archives and the native Windows
  fixture cross-built with pinned LLVM-MinGW 20261006 and warnings as errors.
  Both NDK guest libraries and changed Bionic/translator units also compiled.
- ASan cannot qualify this intentional bad-pointer fixture through glibc: its
  `getxattr` interceptor calls its own `strlen` on the invalid name before the
  kernel can return EFAULT. The unsanitized and UBSan runs retain these tests.
  No ASan pass is claimed for this fixture.

`xattr_fd_host_test` uses real Windows DACL changes after fd admission, separate
EA/data denials, restricted and low-integrity impersonation tokens, classic
Windows delete-pending state, and 1,000 actual reads with exact process-handle
balance and elapsed-time output. It is registered in the existing full Windows
track. The dedicated `xattr fd component` workflow runs on the topic branch and
PRs, freshly builds both Windows architectures from production sources, records
source/compiler/payload identity, verifies the exact current-run artifact, and
executes x64 on Windows Server2022 and2025. ARM64 is cross-built by this component;
actual guest ABI execution remains in full CI.

### First actual Windows gate and fixture repair

The source-bound [Windows gate run 44168127340](https://github.com/gesher-runtime/gesher/actions/runs/44168127340)
executed tree `c08c693f2a694de0ed5fc96742f1716e2e2394d2` on Server2022 and
Server2025. Both passed the public/raw FD suite (**75 checks, zero failures**),
but both reported **148 combined checks, 11 failures**. The whole Windows gate
therefore failed. Successful receipt verification and cross-compilation do not
change that outcome. The reported 1,000-read timings are not successful-workload
measurements because the exact-value assertion failed.

One failure was a token-fixture setup defect:
[DuplicateToken](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-duplicatetoken)
returns only `TOKEN_QUERY | TOKEN_IMPERSONATE`, while
[CreateRestrictedToken](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-createrestrictedtoken)
requires `TOKEN_DUPLICATE` on its input handle. The repaired fixture requests all
three rights with
[DuplicateTokenEx](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-duplicatetokenex),
captures each construction step's immediate Win32 error, and retains the old
limited-rights sequence as an explicit negative control. It checks the actual
restricted thread token against both public operations and the manual evaluator.
The original log contained stale POSIX errno, so the original failing Win32 API
and exact error are not represented as observed facts.

The other failures exposed an incorrect fixture assumption: setting deletion
disposition had been treated as proof that empty-name native reopening failed.
The old log did not record that status. It required unconditional policy refusal
and then mutated/deleted `user.base` when native calls succeeded, causing later
`ENODATA` and read-measurement failures. The repaired controls above distinguish
native authorization from direct manual-evaluator coverage and isolate mutations.
The production policy boundary and exact fallback trigger are unchanged.

**The repaired native fixture still requires a new Windows run.** No repaired
token, direct manual-policy, native-route or measurement pass is claimed from the
failed run. The next gate must establish those results on the exact new source.
If native rights have to be upgraded after a classic delete-pending open, Windows
can still refuse the reopen; this follow-up does not invent access to such a
handle. Keep this qualification separate from successful ordinary held-fd cases.

A separate earlier 71-check Android xattr suite failed to create a hardlink with
EACCES, followed by two dependent cleanup failures, both as shell and in an owned
debuggable app profile. This new fixture avoids that unrelated precondition; it
does not turn those earlier runs into full passes or assert an Android hardlink
contract from those failures. No SELinux settings were changed.

## Source-review repair: argument order and storage exhaustion

The error-order baseline is the Android common kernel commit linked above,
`964a54e5e1a0d70cd80bd5a0885a1938463625b1`. It first admits the path or raw fd,
then prepares set flags, the complete attribute name and the bounded input value,
then checks current inode authorization and resolves the namespace handler. Its
user-namespace inode-type restriction also precedes the handler's empty-suffix
check. This is a versioned contract: newer kernel refactorings can copy set/remove
arguments before path lookup. Results from an arbitrary Linux host are not a
substitute for the declared Android baseline. The recorded Android device above
used kernel 6.6.66.

The backend now separates object admission, argument preparation and current
authorization. Path admission pins the actual object with zero desired access;
the subsequent metadata and access opens still use that held object. Thus a
current EA/data denial does not replace an earlier argument-copy error, while an
invalid raw fd still returns `EBADF`. The additional metadata open on path calls
has a cost; no new throughput claim is made. File-identity locking and the
delete-pending authorization boundary remain in effect.

Android's input limit is 65,536 bytes. An input above that returns `E2BIG` before
inode authorization. A valid-sized value that cannot fit the Windows native EA
store returns `ENOSPC` from the existing atomic commit path instead. Create and
replace conditions are evaluated before storage exhaustion, and failed writes
preserve existing attribute bytes. The backend does not claim that every valid
Android-sized attribute fits every Windows filesystem.

The registered Windows fixture adds competing-error cases under real data/EA
DACL denial, invalid raw-fd and missing-path admission, value-copy faults,
create/replace precedence and full-size storage failures. The shared native/guest
fixture adds symlink type versus suffix, value-copy and flag precedence checks.
These are real public-provider calls; no test-only production branch changes the
result. A future exact-source Windows/Android run must execute the new cases.

The later pre-repair [Windows run 44183195886](https://github.com/gesher-runtime/gesher/actions/runs/44183195886)
passed 249 combined checks on Server2022 and Server2025, including the then-current
75-check shared suite. That validates its own source and corrects the pending-run
status in the historical section above; it does not validate this argument-order
repair. On those hosts native empty-name reopening succeeded, so the run proves
direct held-policy evaluator checks but not execution of the public
`STATUS_DELETE_PENDING` exception path.
