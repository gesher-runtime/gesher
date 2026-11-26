# UIA query cancellation progress

This is a separate issue #178 follow-up discovered while examining an unchanged
UIA owner-close timing gate on PRs #250 and #251. It fixes cancellation progress;
it does not complete an entire performance workstream or claim whole-process idle
or physical performance acceptance.

## Problem and behavior

Closing a native window retires its accessibility epoch before joining the window
owner. The query pool already marks matching requests canceled, but its caller
waits only for `done` and receives no result notification on retirement. If the
JNI callback is blocked on a Java monitor held by the closing caller, that caller
still waits its entire three-second deadline even though the result is already
known to be unavailable. Queued requests removed on retirement have the same wait.

Cancellation now sets its predicate under the request's result mutex and notifies
the result condition variable. The caller returns unavailable when canceled or
when its original deadline expires; an actual successful result still requires
`done`. Retirement notifies both active requests and matching queued requests.
Shutdown uses the same cancellation notification, while retaining its existing
bounded wait for real callback and worker completion.

The bridge publishes epoch death before entering the pool. Every prune now also
notifies the inactive queued requests it removes: an unrelated retirement, new
submission or worker must not erase a request before its matching Retire call
can wake its caller. Independent review reproduced that interleaving on the first
candidate, with a queued caller still delayed 3000 ms; the correction returns it
promptly while preserving all running callback owners.

Cancellation does not set `done`, detach a JNI worker, clear its active slot, release
its shared request, or create a replacement worker. `Worker` retains those owners
until the callback actually returns. The four-worker and 32-queued-request limits,
three-second deadline and original 3500 ms real owner-close gate are unchanged.

## Synchronization

Retire and Stop hold the pool mutex, then briefly take each request's result mutex
to publish cancellation. Worker releases its result mutex before acquiring the
pool mutex. Run releases the pool mutex before waiting on the result condition;
its timeout path releases its result mutex before trying the pool mutex and
pruning, so it cannot re-lock its own result mutex. No JNI callback runs
under either lock. The canceled predicate is serialized with wait registration,
so a notification immediately before the caller waits cannot be lost.

The worker clears `active_` before destroying its loop-local shared pointer. Tests
observe that final lifetime separately from the active count; neither cancellation
nor an empty active counter is used as proof that every reference was destroyed.

## Evidence and limits

The complete production QueryPool host probe has four callbacks blocked at a
declared execution boundary and one genuinely queued request. On unchanged main
`638e19b4a6c109ec8881ba90a94d64b1817fc095`, all retired callers wait approximately
3000 ms and the prompt-return assertion fails. On the corrected owner, they return
within the same millisecond in this host run; all four running requests remain
owned until their callbacks are released, then every request and thread handle
retires without a late effect.

The host suite also exercises 128 retirement/admission/wait-registration races,
128 completion/cancellation races, and 128 successful fresh HWND generations.
It includes the bridge prepublication/unrelated-prune interleaving and a real
three-second queued timeout while four canceled callbacks remain blocked. The
latter must return and prune itself without a recursive result-mutex deadlock.
It compiles the complete production header and implementation. Only Win32 thread
creation, join and handle operations are mapped to host C++ threads; this is not
Windows, ART or desktop teardown evidence.

```sh
python3 tools/test_uia_query_retirement.py --sanitize
python3 tools/test_uia_query_retirement.py --sanitize \
  --source-ref 638e19b4a6c109ec8881ba90a94d64b1817fc095
python3 tools/test_uia_query_retirement.py --sanitize \
  --source-ref 2deca7a3bd5021cf15f510906c072db51cd99785
```

The second command is the negative control and must fail its prompt-retirement
assertion. The third compiles the first candidate and fails only the new
prepublication/prune assertion (2999 ms); the corrected complete suite passed
1194 checks, including that case at 0 ms. In an executor that traces child
processes, LeakSanitizer cannot run;
the local run used `ASAN_OPTIONS=detect_leaks=0`, while retaining ASan/UBSan and
explicit request/handle retirement checks. Ordinary CI retains sanitizer defaults.

The existing real Windows native lifecycle fixture now verifies prompt retired
and shutdown caller return while callbacks still own their workers. Its original
retirement case includes epoch prepublication followed by unrelated pruning. Its
queued deadline case also exercises the timeout lock ordering. The original
deadline, saturation, reentrancy, partial-start, HWND generation, failed-stop and
actual handle-exit controls remain. It is wired into the Windows 2022/2025 host
matrix. The actual production pool and fixture cross-link locally with the pinned
LLVM MinGW compiler and warnings as errors. The full source-pinned ART/Win32 query
boundary gate and original unwrapped deadlock control remain unchanged.

Actual CI traces that prompted this investigation contained about three seconds
in the query wrapper followed by 578–589 ms in real window teardown. This change
addresses the avoidable first interval. It does not identify or claim to repair
the separate, uninstrumented Windows teardown interval. A controlled unchanged-head
retry of PR #250 passed its original gate on both Windows versions. Target CI must
still validate this new cancellation behavior; local host results do not replace it.
