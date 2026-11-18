# ELF symbol versions

The loader reads `DT_VERSYM`, `DT_VERDEF`/`DT_VERDEFNUM`, and
`DT_VERNEED`/`DT_VERNEEDNUM`. Relocations retain the requesting symbol's version
identity. `dlvsym(handle, name, version)` forwards all three arguments through
the Bionic System V ABI; ordinary `dlsym` uses the same graph traversal without
a version request.

## Android contract

The reference is Android 16 r4 Bionic commit
`f22516cbc67e81c13cf943ce02f01b8929a98726`:

- [`linker_soinfo.cpp`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/linker/linker_soinfo.cpp):
  `check_symbol_version`, `find_verdef_version_index`, and GNU/SysV hash lookup.
- [`linker.cpp`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/linker/linker.cpp):
  `VersionTracker::init_verneed`, `init_verdef`, and `do_dlsym`.
- [`linker_namespaces.cpp`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/linker/linker_namespaces.cpp):
  relocation global-group membership uses `DF_1_GLOBAL`; namespace sharing and
  DEFAULT symbol visibility have their separate `RTLD_GLOBAL` rules.
- [`linker_relocate.cpp`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/linker/linker_relocate.cpp):
  per-symbol version lookup, source-symbol cache identity, weak imports, and
  unresolved weak TLS relocation values.
- [`dlfcn_test.cpp`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/tests/dlfcn_test.cpp):
  explicit and ordinary versioned lookup tests.

| Request and provider | Selection |
| --- | --- |
| No version, or a null version pointer | A definition without the VERSYM hidden bit; the usual default version is visible. |
| Explicit name present in VERDEF | Match both its ELF hash and full name, then its VERSYM index. A hidden nondefault alias is eligible. |
| Explicit name absent from a provider's VERDEF | Only `VER_NDX_GLOBAL` definitions can match. Another named default version cannot substitute. |
| Provider has no VERSYM table | Its actual unversioned definition may match an explicit version request. |
| Nonnull empty version string | An explicit request for that string, not ordinary lookup. |

Both symbol-name and version-name hash collisions continue to the next candidate
when the complete identity differs. Requirements are local to each invocation
and relocation symbol index; Gesher adds no persistent name-only lookup cache.
The existing breadth-first handle traversal and architecture filtering remain.

An absent strong versioned import fails loading even when ordinary unresolved
import diagnostics are enabled. It cannot receive a generated function stub or
data placeholder. An absent weak symbol resolves according to its relocation
ABI after version-aware lookup. A weak flag on a version requirement does not
turn a strong symbol into a weak symbol.

## Real and virtual providers

A VERNEED provider must match a direct dependency's SONAME. A virtual dependency
must identify an actual registered provider with owned exports and declared
dependencies. `InstallBionicShim` registers those providers in the common ELF
graph. A symbol callback alone cannot establish provider identity or make a
dependency available in a namespace.

Virtual providers remain unversioned. Their actual exports follow Bionic's
unversioned-provider allowance; this does not create VERDEF records or claim
that a named ABI version is implemented. Matching does not constrain all symbol
interposition to the VERNEED provider: the pinned Bionic matching path also
permits actual unversioned definitions in the lookup scope. The
[namespace contract](native-namespaces.md) controls provider visibility, caller
scope and `RTLD_NEXT` ordering.

The ARM guest libdl path preserves the requested version when reaching the
ordinary loader. Existing architecture-specific unversioned exports retain
their actual ARM implementations; a genuinely selected versioned ELF definition
is not replaced merely because its name also names a builtin.

Diagnostics distinguish a missing provider from an unresolved symbol/version.
Shim error text is owned by the calling thread and consumed once by `dlerror`;
successful lookups preserve an unconsumed prior error.

Symbol selection is independent of the resolved address. In the pinned
`do_dlsym`, a defined IFUNC whose resolver returns zero is still a successful
lookup. The loader's optional `LookupStatus` output distinguishes absence,
resolved addresses and selected symbols whose storage is unavailable, without
executing the resolver again. Ordinary and versioned shim lookup share the
namespace/dependency search, including eligible registered providers. There is
no aggregate builtin retry after that search. A successful null result neither
becomes a different same-name builtin nor introduces or overwrites `dlerror`.
Global relocation lookup also retains the selected symbol when its IFUNC result
is null. Guest `dlvsym` forwards the selected null unchanged and does not consume
the host's error state to guess whether it succeeded.

A selected TLS symbol needs actual current-thread storage. If the TLS backend
cannot provide it, lookup reports address unavailability and sets `dlerror`;
it cannot fabricate success or retry a same-name builtin. This applies to the
native ARM backend's explicitly unavailable attachment path as well as a failed
TLS allocation. A null IFUNC remains a successful result with different status.

Metadata belongs to the mapped library. Parsing, provider-validation, and
relocation failures use the same private graph rollback as
[atomic native loading](native-load-transactions.md). No incomplete handle or
constructor becomes public, and a repaired provider can succeed on retry.

## TLS

Public lookup of `STT_TLS` returns the selected object's address in the current
thread. x86 uses real `TlsGetAddr` storage. Translated ARM uses the current
translator `Env` block, so lookup pointers and guest TLS instructions access
the same bytes. First lookup initializes a new thread; repeated lookup retains
its mutations.

Relocations preserve module/offset semantics. An imported x86 `DTPOFF64` uses
the selected definition's offset, which can differ between two versions of one
name. An absent weak general-dynamic x86 definition has module ID zero; it must
not alias the requesting object's TLS. An absent weak initial-exec relocation
has zero offset relative to TP. ARM's unresolved-weak TLSDESC resolver returns
the descriptor addend minus TP, so the caller's addition of TP produces the
specified null-plus-addend address. Its non-result registers remain preserved.

The translator interface is version 5 because it adds TLS-address and weak-TLS
resolver callbacks. Rebuild the loader and `gesher_arm64.dll` together. Older
interfaces are rejected before appended fields are accessed.

The separate native Windows ARM bridge still lacks an established thread-pointer
attachment path for native ELF TLS instructions. Its new callbacks therefore
return unavailable; they do not manufacture unrelated TLS storage. The
translated ARM path is the target of the Windows ARM fixture gate. This native
execution limitation remains visible in the wider compatibility work.

## Reproducible checks

Original C sources and version scripts produce valid libraries. No fixture
modifies binary metadata after linking:

```sh
bash tests/native/build-elf-versions.sh <C-compiler> <fixture-root>
elf_versions_test <fixture-root> <case> [--arm64]
```

Each case needs a fresh process because loaded SONAME identities persist:

| Case | Behavior exercised |
| --- | --- |
| `lookup-gnu`, `lookup-sysv` | Default/hidden aliases, symbol/version hash collisions, alternating versions, data, weak definitions, and real IFUNC resolvers. |
| `required` | A valid V1-only provider fails a V2 requirement; no constructor/public graph survives; a complete provider succeeds on retry. |
| `weak`, `weak-present` | An absent requested weak function binds zero, or the matching version binds when present. |
| `unversioned`, `global` | No-VERSYM and global-index fallback, including tagged definitions that must remain unavailable for another version. |
| `virtual` | A symbol callback does not invent provider identity; an explicitly recognized unversioned provider supplies only actual exports. |
| `tls` | Distinct-offset TLS versions, relocated client calls, public pointers, mutual writes, and current/new-thread isolation. |
| `tls-weak`, `tls-weak-present` | General-dynamic/TLSDESC weak selection, preservation of the requester's own TLS, and actual initial-exec GOT offsets. |

On Linux x86-64:

```sh
python3 tools/test_elf_versions.py
python3 tools/test_elf_versions.py --sanitize
python3 tools/test_elf_unavailable_tls.py --sanitize
python3 tools/test_elf_dlvsym_shim.py --sanitize
python3 tools/test_arm64_dlvsym_dispatch.py --sanitize
python3 tools/test_arm64_tls_address.py --sanitize
```

The version runner compiles the complete production ELF translation units
unchanged with the explicit Win32 primitive boundary from
`test_elf_atomic_load.py`. It prints complete source/fixture hashes before
compilation. ASan/UBSan findings are fatal; LeakSanitizer is explicitly disabled
for the ptrace environment. The shim unit uses actual extracted dispatch/error
functions and declared lookup seams; it does not establish ELF parsing itself.
The version runner additionally links those complete ELF units to the actual
shim dispatch/error/export section. Original version-script DSOs provide a
null-returning IFUNC, a same-name `getpid` collision, a resolver invocation
counter and a separate global-import client. The dedicated `null-global`
provider is linked with `-z global`: Bionic's relocation global group requires
`DF_1_GLOBAL`, whereas `RTLD_GLOBAL` alone makes an object visible to DEFAULT
lookup. The joint check covers direct, dependency, global and explicit scoped
status; once-only IFUNC execution; absence of fallback; pending-error lifetime;
and a successful null relocation. Windows primitives and the builtin catalogue
are explicit host boundaries. In the complete native shim, DEFAULT preserves
an earlier actual libc export; the same-name null IFUNC check uses its explicit
ELF handle, while the unique `gv_null` also checks DEFAULT lookup.
The unavailable-TLS gate requires Clang and LLD (or `--arm-cc` naming a suitable
Clang command). It builds a valid data-only AArch64 DSO and runs complete
production ELF lookup, actual shim dispatch and the actual native TLS registry
and address methods. Guest instruction entry points abort if reached. Real
publication followed by the backend's unavailable address must produce a failure,
including when the selected symbol has the same name as a builtin. This proves
the host-executed error route, not native ARM instruction or Windows execution.
The ARM dispatch unit similarly tests actual guest libdl dispatch with an
observing host boundary, including genuine ELF selection versus builtin data.
The ARM frontend test compiles that complete production translation unit with
an observing DLL boundary. Actual translator storage and instructions require
their separate lifecycle/runtime checks.

`test_elf_versions.py --source-ref <ref>` compiles the complete historical ELF
tree with an old-API driver. It loads a genuine hidden-only definition and
checks ordinary `Lookup` cannot expose it. The pre-versioning base compiles and
fails that assertion. The shim historical control separately catches dropping
`dlvsym`'s third argument.

Windows tracks 40 and 47 execute the graph driver on Android x86/ARM fixtures.
`elf_dlvsym_test <fixture-root> [--arm64]` links the actual shim and calls its
libdl exports, including TLS and thread-local errors. The ARM variant also runs
guest code calling guest `dlsym`/`dlvsym`, checking architecture-specific exports
and genuine versioned definitions whose names coincide with builtins.
Both native variants include the null IFUNC/error/relocation checks, and the
guest caller includes successful null results with and without an earlier
unconsumed error. Compiling these new cases does not establish Windows or
translated ARM execution; an exact-source native run is still required.

Host execution and Windows cross-linking do not establish Windows execution or
an Android guest oracle. Native glibc characterizes ordinary fixtures, but its
upfront weak-version/global-index checks differ from Bionic. Review actual
Windows gate results separately before claiming runtime coverage; this change
does not close issue #56.
