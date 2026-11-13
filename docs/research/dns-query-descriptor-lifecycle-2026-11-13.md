# Raw DNS query ownership: deterministic close checks

## Finding and correction

The full Windows 2022 test suite exposed a race-prone check in the Web3 native
caller: it received the expected DNS packet and resolver code, then required
`fcntl(old_fd, F_GETFD)` to return `EBADF`, but observed a valid descriptor.
That trace does not identify the open description at that number. The
deterministic control below proves that a verified close followed by legitimate
reuse produces this false failure. The same assumption appeared after cancellation.

The [NDK networking contract](https://developer.android.com/ndk/reference/group/networking#android_res_nresult)
says that `android_res_nresult` closes its descriptor before returning.
[POSIX/Linux descriptor lifetime](https://www.man7.org/linux/man-pages/man2/close.2.html)
allows another thread to reuse the number after close. A later `F_GETFD`
therefore cannot distinguish a leak from a new descriptor in the app process.
The production `gesher_res_nresult` and `gesher_res_cancel` already retire their
query registrations and close the query descriptor; this correction changes
tests and their ownership bookkeeping, not the networking implementation.

`tests/native/web3_network.c` now retires its local descriptor immediately after
calling either consuming API and performs no further syscall on that number.
A saved number in failure messages is diagnostic data only. A failed poll in
the small-buffer path retains local ownership so the existing exit path cancels
the unfinished query. Previously that path set the descriptor to `-1` even
when the result API had not been called.

The caller continues to require the response size, response bit, resolver code,
request ID, NXDOMAIN result, `EMSGSIZE` with unchanged resolver code, invalid
query errors, cancellation, and the original Windows proxy/HTTP markers.

## Replacement ownership coverage

`tools/test_dns_query_lifecycle.py` compiles exact extracted production
`Query`, query registry, `Transfer`, `gesher_res_nresult` and
`gesher_res_cancel` bodies. It seeds real local socketpairs and controlled
completion packets. There is no external DNS dependency and no unrelated
descriptor allocator during the isolated close observations.

The close observation delegates to real POSIX `close`. The fixture snapshots
`F_GETFD` immediately after the consuming call, before assertions, logging or
allocation. It verifies:

- Successful and NXDOMAIN results copy exact response bytes and resolver codes.
- Short buffers, null outputs, worker errors, completion EOF and malformed
  packet sizes consume the descriptor while preserving error outputs.
- Cancellation retires registration and closes the query descriptor exactly
  once. Its active endpoint is shut down to interrupt a receive while the
  endpoint descriptor remains owned by the worker.
- A non-query descriptor, including a newly owned ordinary descriptor at a
  retired number, is not closed by a rejected query lookup or cancellation.

The app fixture is independently exercised using its exact result helper,
cancellation block, small-buffer block and exit-cleanup block. Its NDK adapter
calls the extracted production functions, first proves the query was closed,
then deliberately reuses the number with `dup2` before returning to the caller.
The replacement remains live and transfers a byte through its peer. Thus
forced reuse cannot conceal a missing production close.

Only query setup, close observation, NDK forwarding and a controlled app poll
timeout are seams. The app code is C embedded in a C++ harness; a narrow
diagnostic scope permits C's omitted-field zero initializers without changing
the extracted bodies. These checks do not execute Windows DNS, ART, a guest
ABI or an HTTP proxy. Track 54 retains those real integration paths for x86-64
and translated ARM64.

## Results and controls

| Fixture | Result |
| --- | --- |
| Production ownership cases | 103 checks, zero failures |
| Exact app caller fragments, including forced reuse and failed-poll cleanup | 85 checks, zero failures |
| Generated result-close omission | Compiles; rejected by both exact-close assertions |
| Generated cancel-close omission | Compiles; rejected by both exact-close assertions |
| Actual preceding app source at `4f6febce166b33b3551266e9d9a683a74fea3ad3` | Compiles; 82 checks, seven intended failures |

The old-source failures cover valid response/NXDOMAIN rejection after reuse,
cancellation rejection after reuse, and lost ownership/cleanup after a failed
poll. Conditional observations explain the old/new check-count difference.
The preceding source is read directly from Git; new ownership helpers are not
substituted into it.

Both close-omission controls are mandatory in the ordinary runner. A control
must compile, exit with the assertion-failure code, print exactly the two
expected ownership failures and a completed summary, and have no diagnostic
stderr. Compilation errors, sanitizer failures and unrelated assertions do
not qualify as successful controls.

The positive cases and both generated controls passed with ASan/UBSan. Local
execution disables LeakSanitizer only for the executor's ptrace limitation;
the script and CI do not disable it. The CI preflight step now requires this
runner in addition to the timer and media lifecycle fixtures. Exact published
head Windows results are recorded in the PR after the full rerun.

## Reproduction

```sh
python3 tools/test_dns_query_lifecycle.py --sanitize
python3 tools/test_dns_query_lifecycle.py --case app \
  --app-source-ref 4f6febce166b33b3551266e9d9a683a74fea3ad3
```

The second command is an expected failing control. The runner prints SHA-256
hashes of the exact production and app inputs. Runtime and SDK inventory
measurements from the accompanying lifecycle audit are unchanged by this
test-only correction.
