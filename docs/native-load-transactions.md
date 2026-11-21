# Atomic ELF load graphs

The native loader implements N01 from [issue 56](https://github.com/gesher-runtime/gesher/issues/56):
a failed load must not leave a partially linked object available through a later
load, symbol lookup, or program-header iteration. Missing dependencies,
relocation failures, and protection failures retire resources owned by that
attempt. A retry uses the files and bindings available at the time of the retry.

## Android reference and transaction boundary

The source reference is Android 16 release 4 Bionic, pinned to
`f22516cbc67e81c13cf943ce02f01b8929a98726`.
[`find_libraries`, `find_library`, `do_dlopen`, and `soinfo_unload`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/linker/linker.cpp)
discover, map, and link the dependency group before constructors, and release
new objects on failure. TLS ownership is released before object destruction.
[`dlopen_ext` and `__loader_dl_iterate_phdr`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/linker/dlfcn.cpp)
use the recursive loader mutex. These are source-contract references, not
execution of the Android reference linker.

Each Gesher `Load` or `LoadFromMemory` holds the recursive loader mutex. Its
private `LoadTransaction` maps the complete graph, prepares TLS and rewrite or
ARM registrations, relocates dependencies before dependents, and completes
protections and deferred-code registration. Only then does it publish the graph
and run dependency constructors. A constructor can reenter on the same thread;
foreign observers wait for the complete operation, including constructors.
After each node's relocation, its TLS image is prepared privately for the
loading thread. A parent's IFUNC can read a linked child's initialized TLS and
retain its writes through final publication. Other existing threads receive
the original image only at publication; later threads also start from that
image. Preparation does not make the module public or prevent rollback.
This follows Bionic's
[`register_soinfo_tls`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/linker/linker_tls.cpp)
before linking and
[`tls_get_addr_slow_path`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/bionic/bionic_elf_tls.cpp),
which copies the initial image on first access and retains the thread's copy.
Copied executable segments receive their requested protection before relocation
too: a real IFUNC resolver executes at that stage. Text relocation pages can
temporarily become writable and regain their final protection afterward.

Failure withdraws registrations before unmapping their owner:

| Resource | Cleanup |
| --- | --- |
| New graph nodes | Release their mappings and file images; retain borrowed committed objects. |
| x86 TLS | Remove unpublished identities and cached bases, free dynamic copies, clear static bytes, and reuse aligned gaps without moving live modules. |
| ARM TLS | Retire unpublished reservations, clear their bytes, and reuse aligned gaps below newer committed reservations. |
| x86 rewrites | Remove ranges, traps, syscall sites, fingerprints, pending pages, and initial/deferred stub arenas before freeing names or mappings. |
| ARM code | Remove owned ranges and entry thunks; invalidate thread classification and Jit code caches before execution, including resumed nested calls and pooled environments. |
| Unresolved-import diagnostics | Release owned function stubs, ARM bindings, inaccessible data reservations, and references to diagnostic names. |

An IFUNC can successfully load an independent object in a nested transaction.
That committed object survives a later outer failure, with its TLS offsets and
per-thread state intact. Rollback does not reverse arbitrary resolver actions.
Committed objects retain the existing `Close` lifecycle; this change does not
introduce general unloading of successful mappings.

Published x86 TLS addresses use a thread-owned cache. A warmed lookup does not
acquire the process-wide module registry mutex, so unrelated allocation,
publication or rollback cannot serialize every TLS access. Provisional entries
remain in a separate locked map with the owner check; their existing map nodes
move into the published cache on first use after publication. Rollback never
touches the published cache. This depends on the current published-module
lifetime and nonreused IDs, and does not add successful unloading. Any later
unload implementation must supply cache invalidation before retiring storage.
The pinned Bionic `__tls_get_addr` similarly keeps a validated cached path
outside its registry lock; its broader DTV generation/unload machinery is not
claimed here.

### IFUNC reentry into a pending object

A request for an object still pending in an outer transaction returns an
explicit recursive-load diagnostic. It does not expose an incomplete handle or
wait for itself. This is not a claim of Android's exact error behavior. In the
pinned Bionic source, such a request can find a pending `soinfo`, but a fresh
`readers_map` supplies a default unread `ElfReader` for that unlinked object.
`ElfReader::Load` requires `did_read_` to be true. See
[`LoadTask::get_elf_reader` and `find_libraries`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/linker/linker.cpp)
and [`ElfReader` construction and `Load`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/linker/linker_phdr.cpp).
This path reaches a failed precondition, not an established successful reentry
contract. Dependency cycles, constructor reentry, and independent nested loads
are separate supported cases.

## Reproducible validation

On Linux x86-64, with C++20 and a native-TLS C compiler:

```sh
python3 tools/test_elf_atomic_load.py
python3 tools/test_elf_atomic_load.py --sanitize
python3 tools/test_elf_tls_rollback.py --sanitize
python3 tools/test_arm64_pending_rollback.py --sanitize
python3 tools/test_arm64_native_rollback.py --sanitize
```

The atomic runner snapshots and hashes the complete production ELF tree before
compiling it. Its real original DSOs contain `PT_TLS`, dependencies,
constructors, and IFUNC relocations. Cases cover missing direct/transitive
dependencies, repeated memory-load failures, repair and retry, borrowed
dependencies, relocation and protection failures, diamonds, cycles, constructor
reentry, file/memory IFUNCs, initialized dependency TLS in IFUNCs, and concurrent
loads and observers. A separate
executable redirects only the production ELF translation units' `mprotect`
calls through a controlled one-shot failure boundary. The ordinary production
target has no failure switch.

Ownership units include the complete production rewriter or loader to inspect
private metadata without shipping test APIs. They verify allocated arenas,
diagnostic ownership, fingerprints, pending pages, preparation failure cleanup,
and preservation of another owner. These are explicit resource units; the graph
driver covers public transaction paths.

The Linux boundary uses real `mmap`, `mprotect`, threads, and locks. It does not
execute Windows VEH, TEB/GS addressing, FLS, or stack commitment. ARM rollback
host tests compile production sections with explicit platform/Jit seams; these
tests alone are not ARM translation execution. Sanitizers cover compiled host
code, not uninstrumented ELF fixture instructions. The atomic runner disables
LeakSanitizer under ptrace; resource counts and reuse remain explicit checks.

The TLS host fixture also pauses a real production allocation at its explicit
Win32 boundary while the registry lock is held. An existing thread must still
read its warmed static and dynamic published TLS before that pause is released.
The prior implementation fails this bounded control. A separate concurrent
case interleaves published reads with 32 pending rollbacks, publications and
thread teardown. Timing a Linux adapter remains distinct from Windows timing;
these controls establish cache independence and preservation, not an application
throughput guarantee.

The historical negative control uses the complete old implementation and the
current graph/boundary, stopping before a partial handle can execute:

```sh
python3 tools/test_elf_atomic_load.py --source-ref 7b50854206a2b3e8f7b4266a132285d6449546f6
```

The expected rejection is the old failed graph remaining visible through
`Find`. Record it as a negative control, not a positive test pass.

`native-libs.yml` and `arm64.yml` build the graph with API 29 or later NDK
compilers for native TLS. Windows tracks 40 and 47 run both
`elf_load_transaction_test` and `elf_load_transaction_failure_test`; track 47
uses the production ARM translator. The driver accepts the graph directory and
optional `--arm64`. CMake builds the failure variant from the real loader and
keeps the ordinary production target separate.

The host and ARM translator must be built together with API version 4, which
adds rollback and private TLS preparation. Mixed version-3/version-4 binaries
are rejected before accessing the appended API fields; they are not compatible
fallback configurations.

Cross-compilation and linking establish build integration only. Report the
exact candidate commit, source hashes, compiler, command, result, and runtime
environment. Keep Windows execution and matching Android reference execution
distinct from host results.
