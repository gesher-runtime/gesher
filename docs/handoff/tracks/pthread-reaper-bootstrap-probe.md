# Explicit-bootstrap callback-release experiment

The opt-in `probe-reaper` supplier role adds `GESHER_REAPER_PROBE` alongside
the existing routing probe. The production, fault, routing, routing-fault and
original-recheck roles keep that macro off; their existing twelve jobs remain.
Three additional jobs build and verify a separate source/run/attempt-bound
supplier, static host and callback DLL on both Windows runners.

This is a bounded experiment, not canonical product activation. Normal host
code must start the worker before **any** key creation in this role. Missing
bootstrap fails with EAGAIN and leaves the caller key output unchanged. Event
and thread creation failures are separately injected; rollback and a subsequent
successful start/stop are checked in fresh processes. Neither constructor nor
TLS callback starts or waits for this worker.

Each key-generation allocation contains an aligned queue node. Final release
enqueues module-bearing generations and signals the already-created event;
the worker pops, releases the module, frees the generation and decrements the
outstanding count. The callback DLL's nonblocking detach observer records the
actual DLL_PROCESS_DETACH thread ID. The fixture requires it to equal the
reaper ID and differ from the native exiting thread, after a real held callback,
key deletion, callback return, thread exit and physical DLL unload. Stop is a
test-only operation after joining the sole producer and observing zero records;
it is not a concurrent shutdown protocol.

Canonical startup remains unresolved: gesher-exec creates ProgramThread before
ELF loading, JNI can trigger arm64EnsureLoaded separately, and translator tests
load the DLL directly. Earlier CRT/TLS key creation and dalvikvm startup are not
fully mapped. This experiment does not justify inserting a lazy waiting once,
starting a worker under DllMain, or enabling the macro in any product supplier.

Remaining loader risks include indirect allocator/CRT calls, arbitrary callback
behavior, module lookup in key creation and clock discovery, nonlocal exits and
resource leases for cancel/join targets after registry unlock. Record retention
alone does not retain the target's closed handles or clock allocation. Actual
guest ABI/consumer binding, selective canonical supplier adoption and full CI
remain separate requirements.
