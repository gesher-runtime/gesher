# Broker process and thread ownership

`ServeProc` and the live process registry own the process. Each admitted
`ServeThread` connection owns its thread and pins its process. The process's
thread and waiting lists are non-owning and protected by `g_lock`.

Transactions own their `from` and `to_thread` participants. Existing transaction
retirement paths clear these references; any remaining transaction can keep its
dead participant alive until that transaction is freed. This prevents dangling
references without retaining every process/thread for the broker's lifetime.
Nodes clear their process association on retirement; refs and buffers are
released by `ProcRelease`. The unused transaction process pointer is removed.

`tools/test_binder_lifetime.py --aosp-dir <aosp> --cxx <llvm-mingw-clang++>`
compiles the entire production broker and runs real Windows named-pipe process
and thread churn. It measures outstanding C++ allocations after retirement,
closes a process during an actual blocked `ThreadRead`, covers early pipe close
and deterministic handshake-write failure, and checks that a pending
transaction owns its participants until destruction. The last ownership check
constructs participants directly; it does not substitute the broker's transport
or retirement functions. `--source-ref origin/main` is a failing control.

CI cross-compiles this executable and runs it in the Windows services track,
beside the existing cross-process `binder_probe`/`libbinder_probe` checks for
nested calls, object identity, death notifications and dead replies. Local
validation ran `binder_probe` with the changed broker and the previously built
POSIX library. Full ART, libbinder and scatter/gather CI remain separate gates.
