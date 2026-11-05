# Telegram native networking observation, 2026-11-05

The reported login spinner did not reproduce as a failed native networking call
in the observed running process. No production socket/epoll change is justified
by this observation. The initial cause remains unproven.

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
successful inspection. A later bounded stack snapshot showed the Java main
thread in its normal Looper poll and the native window thread in its message
wait; it did not establish a window deadlock.

Local evidence is under `out/telegram-network/`. `evidence-redacted.json`
contains only binary identities, call counts, connection-success observations
and restoration status. Full application logs, phone input, captcha tokens,
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
