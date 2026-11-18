# Connectivity callback host checks

`tools/test_connectivity_callback_contracts.py` compiles the changed production
ConnectivityManager and ConnectivityService. `--case timeouts` checks removal of
an Integer.MAX_VALUE-millisecond deadline after unregister and first availability,
including the initial online snapshot. Existing tests cover stale timeout delivery,
callback reuse, loss after satisfaction, reentrant unregister and service teardown.

The controlled Handler uses a real Android legacy MessageQueue linked list;
Handler.removeCallbacks and MessageQueue removal/recycling execute unchanged.
Enqueue/dispatch is deterministic and tracing is disabled at explicit JNI seams.
No native poller, real Looper scheduling, Windows network discovery or cross-process
Binder behavior is claimed by the host run. Windows ART fixtures additionally run
their existing actual HandlerThread and generated Binder/Parcel checks.

On Windows pass `--cc <llvm-mingw C compiler>` and a Windows `--java-home`.
The unrelated notification descriptor read seam returns ENOSYS on that host.
The baseline control is `--source-ref origin/main --case timeouts`; it fails at
`unregister retained a long deadline` before the fix.
