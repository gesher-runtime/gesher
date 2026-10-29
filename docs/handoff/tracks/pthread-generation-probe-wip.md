# Process pthread generation probe — scope and open work

This branch is an experimental CI-only supplier role. It is not an adopted
product provider and must not be enabled in production. The candidate adds a
non-reused key generation record, synchronized TLS slot generations, deferred
record recycling while cleanup holds a target, and ordinary-return callback
references. Key operations use key lock -> registry lock -> thread spin lock;
user destructors execute after releasing these locks. An experimental callback
module reference is acquired before the key lock and released after the last
ordinary callback returns and the key owner is deleted.

The tests compare Linux self-delete and concurrent delete/reuse behavior with
Windows, and exercise an actual unloadable callback DLL, same-slot reuse,
four destructor passes, and a forced wrapper detach window. Separate exact
production/fault and probe-fault roles preserve their own receipts and 338/+8
and TLS-clear controls. Their green status must be established from this exact
source; historical runs do not qualify it.

Open: cancellation, pthread_exit, arbitrary longjmp or other nonlocal callback
exits can abandon generation and target references. Safe release may require a
process-owned reaper that waits on the original kernel thread object before
unloading callback code. Thread-loader callback paths may run under the Windows
loader lock; no general lock-order or shutdown proof exists. Direct writes to
public `_pthread_key_dest` do not update generation records. Non-image/JIT
callbacks and DLL-only callback module ownership are not qualified. Complete
winpthread entrypoint routing, prebuilt/static third-party providers, both guest
ABIs, supplier adoption and the full Windows/package/e2e gates remain open.
No passing scoped test may be used to claim these contracts are closed.
