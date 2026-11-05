# Telegram native networking observation, 2026-11-05

The reported login spinner did not reproduce as a failed native networking call
in the observed running process. No production socket/epoll change is justified
by this observation. The initial cause remains unproven.

A subsequent native crash of that same process at 02:30:56 remains unresolved;
the successful network observation and suspension recovery do not establish
overall Telegram stability.

## Exact observed configuration

- Telegram `org.telegram.messenger.web`, version 13.1.1, versionCode 72379.
- Native engine: `libtmessages.49.so`, x86_64, loaded as a manually mapped ELF
  library (not a Windows DLL).
- Existing SOCKS5 setting: `127.0.0.1:1080`.
- Existing shared system server PID 14532 was left running throughout.
- The user reopened Telegram during the investigation: initial PID 31272,
  subsequent PID 30228. The investigator did not restart either process or the
  shared Gesher device, enter a phone number/code, or submit a login request.

Observed file SHA-256 values:

| File | SHA-256 |
| --- | --- |
| Installed `dalvikvm.exe` | `3edbe27954e7b6865fdda88a9df6be93ef2283d86de1e2e4e68267d6727a88c2` |
| Telegram `base.apk` | `148f1aa51b0a76c8168e4ad0eb121a66b78c2b8f50e4dc44e7a667668cf8823b` |
| `libtmessages.49.so` | `178b041bdea1b4e4fd54df073e84d3dcd2c17e691a378e545570d0e8128b8427` |

There is no independently established source receipt for the installed runtime.
Do not attribute the successful observation to the pollfd fix `657fb27a` or to
the current checkout merely from symbol presence, timestamps, or this report.

## Native observations

The first process had five constructed ConnectionsManager objects and four
initialized native worker threads. Their epoll/eventfd pairs were 20/21,
22/23, 24/25, and 26/27. The workers were in the shim's EpollWait wrapper;
native ELF unwinding stopped at that wrapper. Their task queues were empty and
networkAvailable was true. A five-second idle sample counted 15 epoll_wait
calls and no socket/connect calls. An idle sample does not demonstrate failure
of socket(), connect(), or eventfd.

In the subsequent process, the 02:04:00–02:04:30 Asia/Jerusalem sample counted:

| Native call | Observed calls |
| --- | ---: |
| socket | 1 |
| connect | 1 |
| epoll_ctl | 18 |
| epoll_wait | 125 |
| eventfd_write | 1 |

Windows reported an established TCP connection from this process to
127.0.0.1:1080. Temporarily enabled tgnet diagnostics reported the SOCKS
connection, a connection to 149.154.167.92:443 through it, TL_pong responses,
and an RPC response whose constructor was 0x5e002502. The official Telegram
schema identifies that constructor as
[auth.sentCode](https://core.telegram.org/constructor/auth.sentCode).
The user subsequently reported progression; a separate window observation
identified the next page title as “Choose a login email”. Foreground/window
behavior is a separate investigation, not a native TCP failure.

The native diagnostic flag was restored to its original zero value, temporary
breakpoints were removed, and the debugger explicitly detached after every
successful inspection. Those checks were insufficient: the Windows LLDB
stop/detach sequence left manual thread suspensions in place. The subsequent
stack snapshot therefore did not establish a window deadlock or prove that
the app was still progressing.

## Diagnostic suspension incident and recovery

The investigation caused Telegram's temporary inability to come to the
foreground. After LLDB detached, a read-only Windows query found DebugPort 0
but 100 threads with suspend count 3 and one thread with count 2. All 101
thread IDs matched the saved debugger snapshot; their creation times and
counts matched the diagnostic stops. The extra thread was created during the
last debugger attach. A zero debug port does not establish zero manual thread
suspensions.

Automatic approval initially rejected restoring the complete thread set
because ownership of each hold had not yet been sufficiently established.
No ResumeThread calls ran in that attempt. After the user explicitly approved
releasing these suspensions in Telegram only, recovery retained a handle to
PID 30228 and verified its executable path and exact creation FILETIME
134383105751467163. It then retained each thread handle, verified process
ownership, checked the unchanged per-thread count, and released exactly the
approved 302 holds. Every ResumeThread return matched the expected count and
all 101 retained threads were verified at count zero. The process was not
restarted and no other process was modified.

Post-recovery, the same process reported Responding=True and an established
connection to the existing SOCKS proxy at 127.0.0.1:1080. A separate UI check
successfully foregrounded Telegram in under one second and showed the blank
Add Email page. No authentication input was entered. The foreground problem
is closed by recovery of the diagnostic state; no production graphics change
is justified. LLDB was not attached again. The exact before/after ledger is
`out/telegram-network/suspension-recovery.json`.

## Subsequent native crash at 02:30:56

The user reported another crash after recovery. Read-only log inspection
confirmed that PID 30228 terminated with SIGSEGV, code SEGV_MAPERR, fault
address 0x308, and system-server exit code 1. The shared server PID 14532
remained alive and responsive. Faulting thread 5548 was absent from the saved
101-thread debugger snapshot and suspension recovery set. This is not a
breakpoint-trap signature; no causal connection to either the previous
suspensions or the network layer has been established.

The fatal report said `Unknown architecture/word size/OS in ucontext dump`,
with an empty backtrace and empty fault message. The AOSP register dumper in
`art/runtime/runtime_common.cc` selects its x86_64 register path only under
`__linux__`; the compatibility fault handler does provide a Linux-layout
ucontext on Windows. Thus the existing report lacks the faulting PC needed
for source attribution. No matching Windows crash dump or recent WER event
was found. A Windows fatal-register diagnostic is a possible next step,
not a crash fix. Sanitized fatal evidence is in
`out/telegram-network/crash-023056-redacted.json`.

No app was restarted, no further debugger attach occurred, and no process
memory was changed during this crash investigation. Authentication activity
was performed by the user. The crash remains open pending actionable native
context or a controlled reproduction authorized separately.

`patches/art/0771-windows-fatal-context.patch` enables only the Gesher Windows
x86_64 register-dump path, with explicit REG_RAX/RSP/RIP/EFL layout assertions.
It prints raw RIP and registers and attempts PE module+offset attribution via
the existing host dladdr implementation. Manually mapped ELF/JIT PCs remain
raw addresses; the patch does not claim to add native unwinding or fix the
underlying crash. The active AOSP/build tree was not modified during its base
build. A patched source copy compiled successfully with the active native
build's exact compile flags.

The existing codegen test track now runs two disposable JNI fatal fixtures:
a direct ELF null read and a host strlen null read. Both must exit with a
native SIGSEGV and include RIP/RSP; the host case also requires a PE module
offset. Their native fixture compiled with -Wall -Wextra -Werror, the Java
fixture compiled and dexed, and the PowerShell track parsed successfully.
The installed unmodified runtime was run as a negative control in separate
VMs with workspace-only data: both cases exited 1 with SIGSEGV and the unknown
context marker, with no RIP/RSP. The native-build agent then applied the
reviewed patch from isolated source commit 564118b1 and rebuilt successfully.
Both fixtures passed against the fresh `out/native-build/build/dalvikvm.exe`:
exit 1, exact SIGSEGV/SEGV_MAPERR address 0x308, nonzero RIP/RSP, no timeout or
startup error, no unknown-context marker. The ELF case retained a raw PC;
the host case resolved `ucrtbase.dll+0xed171`. Results and synthetic logs are
`out/telegram-network/fatal-patched-results.json` and `fatal-patched-*.log`.
No user app was involved in those fixtures. The original Telegram crash's
cause remains unresolved; these changes improve the next fatal report.

Local evidence is under `out/telegram-network/`. `evidence-redacted.json`
contains only binary identities, call counts, connection-success observations
and restoration status. The original restoration flag covered only the log
flag and debugger detach; it now also requires the successful suspension
recovery ledger. Full application logs, phone input, captcha tokens,
code hashes, and authentication payloads are not copied into that evidence.

## Regression coverage

`tests/native/tgnet_event_loop_test.inc` is included by the existing
`libc_test.c`/native-libs test route. Four cycles start a native pthread through
an eventfd, preserve distinct data.ptr cookies while epoll watches both the
eventfd and a nonblocking TCP socket, check connect completion through SO_ERROR,
and exchange a synthetic SOCKS5 greeting/connect response and PING/PONG with a
loopback peer. Each cycle uses one five-second monotonic deadline. It never
contacts an external proxy/server or uses account credentials.

The focused x86_64 NDK test compiled with `-Wall -Wextra -Werror` and ran in a
separate ART process using the installed runtime and workspace-only test data:
all three checks passed and `TgnetEventLoopTest all passed`, exit 0. The complete
libc_test library compiled with its normal CI flags; its pre-existing format
and unknown-escape warnings remain. This local focused result is not full CI
acceptance of the combined working tree.
