# Genuine compiler C++ TLS baseline

This consumer-only change keeps every cafe2129 supplier patch, private compiler
flag, original artifact and all twenty-one existing gates unchanged. It adds
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
against their exact supplier receipt. Actual link selection is audited from
that transcript, without inferring it from command arguments alone.

Three supplier variants remain distinct, including their artifact names:

* `published`: the exact verified 20261006 compiler archive is freshly extracted,
  including its unmodified shipped winpthreads library. This is a fresh publisher
  artifact contrast, not an attestation of the existing canonical mutable cache.
* `production`: the existing private production role, with its rebuilt authority
  supplier and all probe macros off. This is not the unmodified shipped library.
* `deferred`: the exact existing private deferred-only diagnostic supplier and
  ABI, without rebuilding or modifying it for this consumer test.

The first two reuse baseline_compile; deferred reuses probe_deferred_compile.
Each new C++ artifact has its own complete member/role/source/run/attempt manifest
and exact original producer receipt. The original artifact directories and
member contracts are untouched. A separate Linux reference and six Windows
variant/host consumers bring the total to28 jobs while retaining the original21.

Actual results are required before deciding whether there is a supported late
cleanup identity gap. A source hypothesis about callbacks after TLS clearing is
not evidence that this production compiler uses such an order. Arbitrary MSVC
or FLS interoperability is not added to the supported contract by this fixture.
General asynchronous entry safety, actual process-owned retirement/bootstrap
across supported program entrypoints, supplier/guest consumer adoption and full
product qualification remain open.
