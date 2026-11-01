# Genuine compiler C++ TLS baseline

The initial consumer-only baseline kept every cafe2129 supplier patch and all
existing gates unchanged. Run410884 then exposed an actual deferred-only owned
finish regression. The current candidate corrects that diagnostic supplier's
TLS lifetime; the seven older supplier roles and original21 job definitions
remain unchanged. The baseline adds
three C++ translation units plus a shared header using real `thread_local`
objects with nontrivial destructors. No hook chooses destructor order, no FLS
callback is injected, and no compiler TLS-model option is selected.

The executable exercises pthread start-function return, explicit pthread_exit,
and std::thread return. Windows also exercises a real CreateThread worker that
calls native ExitThread61 and is observed through actual kernel termination.
Linux has the first three supported cases. The two destructor translation units
report their actual sequence, native thread identity, expected/observed pthread
identity, body-exit phase and exact callback counts. Each case requires one
destructor per object and unchanged identity. All cases are logged before the
final conformance result; a conformance failure exits2 and is not accepted as a
green observation. Every Windows worker duplicates its native thread handle
before constructing the TLS objects. Main retains callback state through an
actual kernel-completion wait on that owned handle for every exit mode, even if
the supplier's pthread join publishes completion before later CRT callbacks.
The native ExitThread61 case also retains its original external kernel witness.
Every duplicated handle is closed and Windows retains the original +8 bound.
Immediately after each pthread_join/std::thread::join returns, before any extra
kernel wait, the fixture separately snapshots callback counts and identity
mismatches. Each TU publishes its own completion as the destructor's final action
after the record/log call returns; both completions must be visible at join.
Windows also samples its owned kernel handle with timeout0 immediately at join,
logging signaled/timeout as a separate observation without requiring native
bookkeeping to have finished at that instant. Incomplete callbacks at join remain
a strict failure even if they finish during the later storage-protection wait. The native ExitThread
case has no pthread join; its observations follow actual native termination.

Compiler flags follow the production GNU C++20/no-exceptions/sized-deallocation
and static-link configuration. Debug symbols are retained for actual compiler
TLS registration evidence. The producer stores complete commands, object hashes,
all four source hashes, imports, demangled object/executable symbols, driver
hash/version and target archive hashes. Actual emutls/thread-atexit/TLS symbols
are printed without assuming in advance which mechanism the compiler chooses.
The complete verbose linker transcript is printed and retained as a hashed
artifact member. The explicit libwinpthread.a and the -pthread static alias
libpthread.a have distinct path/hash bindings; private variants validate both
against their exact supplier receipt and both static aliases must be byte equal.
The pinned upstream install-exec-hook copies the built static library to this
alias; an original canonical alias cannot silently fill a private variant.
The actual GNU-style live linker map and loaded archive(member) trace jointly
bind pthread self/create/join/exit/key create/delete/get/set, key cleanup, the
pthread TLS callback and its registration slot to their selected variant's exact
archive hash and member. Ambiguous member basenames across any loaded archives,
missing symbols, conflicting symbol address/owner and other-variant archives are
rejected. LLD repeats resolved global definitions while walking input symbol
tables; only identical symbol/address/owner repeats are deduplicated.
Unresolved actual evidence prints the explicit missing/ambiguous names and full
raw map before failing; optimized or renamed symbols are never silently replaced.
Both raw map and verbose trace remain complete hashed artifact members.
Run410871 compiled and linked the published and deferred consumers, but failed
the original strict parser before Windows execution: this LLD logs full paths
in archive Reading rows and basenames in Loaded archive(member) rows. The parser
now resolves a pthread basename only through exactly one actual full-path Reading
origin; missing, canonical-other-root or ambiguous origins fail. No expected path
is silently prefixed. The original red run and its raw maps/logs are preserved;
Linux's three genuine cases passed; Windows callback outcomes were unexecuted
in that first run. The fresh changed-parser run410884 executed all28 jobs:
26 succeeded, and only the deferred C++ consumers failed on both hosts. Published
and private macroOFF passed all four modes. Deferred owned return changed
pthread identity1 to3 and pthread_exit4 to5 in both genuine TU callbacks, on the
original native thread. Its std::thread/nativeExit61 cases passed. Completion
markers were1/1 and immediate join kernel samples0; this was an identity
regression, not an observed premature join. All original21 jobs passed.

The map format and option are grounded in LLVM's
[MinGW driver](https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/lld/MinGW/Driver.cpp)
and [COFF live map writer](https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/lld/COFF/LLDMapFile.cpp),
with actual CI required to confirm this compiler's emitted format.
The exact upstream
[install hook](https://github.com/mingw-w64/mingw-w64/blob/a3d93999ef0521681d45e445c39964a7af0f593f/mingw-w64-libraries/winpthreads/Makefile.am)
defines the static alias copy.

Three supplier variants remain distinct, including their artifact names:

* `published`: the exact verified 20261006 compiler archive is freshly extracted,
  including its unmodified shipped winpthreads library. This is a fresh publisher
  artifact contrast, not an attestation of the existing canonical mutable cache.
* `production`: the existing private production role, with its rebuilt authority
  supplier and all probe macros off. This is not the unmodified shipped library.
* `deferred`: the separately receipted private deferred-only diagnostic supplier.
  The current changed-source candidate retains its TLS owner through actual
  kernel completion; its calculated private ABI is560/align16, with every field
  offset required from the actual CI compile. It is not the original archive.

The first two reuse baseline_compile; deferred reuses probe_deferred_compile.
Each new C++ artifact has its own complete member/role/source/run/attempt manifest
and exact original producer receipt. The original artifact directories and
member contracts are untouched. A separate Linux reference and six Windows
variant/host consumers bring the total to28 jobs while retaining the original21.

Actual410884 establishes the deferred owned-finish gap and the two positive
archive contrasts. The changed source and held-callback witnesses require a new
whole28-job run before qualification. See the exact
[kernel TLS owner protocol](pthread-kernel-tls-owner.md). Common cases additionally
set/get a late TSD value and report its destructor count as observation only;
this does not qualify late-created TSD cleanup ordering. The two deferred-only
held cases explicitly clear their late value and preserve the original owned
TSD pass budget and outside-loader cleanup location. Arbitrary MSVC
or FLS interoperability is not added to the supported contract by this fixture.
General asynchronous entry safety, actual process-owned retirement/bootstrap
across supported program entrypoints, supplier/guest consumer adoption and full
product qualification remain open.

Run411101/source212579 independently qualified the changed private deferred
identity/resource slice on both hosts, including both genuine held cases and
original total+8. Actual560 layout, all offsets and selected-member provenance
were verified. Aggregate26/2 still fails because the unchanged original publisher
exceeds the strict census during the newly exercised common late-key access;
its identities/completions remain positive. Full result and remaining limits
are recorded in the kernel TLS owner protocol. The subsequent characterization
changes observation/metadata only and retains this supplier source exactly.
