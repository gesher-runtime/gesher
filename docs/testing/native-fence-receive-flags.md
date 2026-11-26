# Native-fence receiver descriptor flags

`recvmsg` selects close-on-exec independently of the sending descriptor. This
applies to a native-fence descriptor received through `SCM_RIGHTS` just as it
does to an ordinary file descriptor. The Android kernel's
[`scm_detach_fds`](https://android.googlesource.com/kernel/common/+/refs/tags/android-13.0.0_r0.55/net/core/scm.c)
chooses the receiving descriptor's flag from `MSG_CMSG_CLOEXEC`.

| Receiving flags | Received `F_GETFD` |
| --- | --- |
| No `MSG_CMSG_CLOEXEC` | `0` |
| `MSG_CMSG_CLOEXEC` | `FD_CLOEXEC` |

The typed native-fence path previously omitted that choice when calling
`AdoptNativeFence`, so its noninheritable reservation made both rows return
`FD_CLOEXEC`. `ReadDelivered` now passes the receiving choice explicitly.
Registration changes the reservation's native inheritance bit while holding
the descriptor-table guard, before publishing its native-fence owner. A failed
flag operation preserves its error, retires the unpublished descriptor/OFD
reservation, and releases the incoming completed event.

Creation/export and Binder/direct adoption retain their existing close-on-exec
default. The event type, immutable completed-event validation, wire kind `5`,
metadata `4`, absent shared-state handle, readiness and ownership are unchanged.

## Existing inheritance boundary

The real completion event remains separate from the descriptor's NUL
reservation. The existing generic startup manifest does not carry that typed
completion capability. Its descriptor kind remains `kUnshareable`, and
`SpawnWithFds` continues to return `ERROR_NOT_SUPPORTED` before creating a
child. Native raw-file export similarly returns `EOPNOTSUPP`. Changing
`F_SETFD` or receiving a descriptor without close-on-exec must not turn that
reservation into an apparently successful inherited fence. This repair does
not add native-fence process inheritance.

## Regressions and source preservation

`tools/test_native_fence_receive.py` extracts the exact production reservation,
registry, registration, retirement, adoption declaration/body and broker-reply
decoder. Its declared Linux adapters model Win32/CRT handles and event
validation; they do not execute Windows object validation, a GPU or the broker.
It checks both sender/receiver flag choices for fences and ordinary files,
unchanged direct-adoption defaults, malformed type/metadata/shared handles,
discarded rights, allocation/reservation/flag failures, error preservation and
handle/OFD/member retirement. The original source is an explicit failing
control:

```sh
python3 tools/test_native_fence_receive.py
python3 tools/test_native_fence_receive.py --sanitize
python3 tools/test_native_fence_receive.py --source-ref 972c2b42b40c4ac799e147f5e352d07ba615d0fb
```

The final command must fail the receiver-flag assertions; it is not a passing
test command. LeakSanitizer cannot run under the local executor's ptrace
boundary. Local ASan/UBSan evidence therefore separately records
`ASAN_OPTIONS=detect_leaks=0`; the committed CI command does not disable it.

On Windows, `egl_fence_test` uses real ANGLE/D3D11 completions and an actual
`binderd` instance. Its stream and datagram SCM paths mix a fence with an
ordinary pipe for all four sender/receiver flag combinations, close sender
references before receive, and verify completion, data/EOF and receiver
cleanup. It also checks `dup`, `dup2`, `dup3`, `F_DUPFD`, `F_DUPFD_CLOEXEC`,
`F_SETFD`, invalid duplication, malformed native adoption, startup selection,
and the explicit unsupported export/launch boundaries. The native-libs track
requires the SCM success marker on both the default device and WARP. Linux
adapter results and Windows cross-link results do not substitute for these
Windows runtime gates.

The source guard leaves `ofd-stream-source.json`, its original patch, the 71
original digests, reviewed checkpoint/hunks, supplier inputs and
`native_runtime_approval=false` unchanged. A separately pinned three-file
transition restores the complete main972 native-fence implementation, header
and rights decoder before the original preservation checks. It verifies the
baseline policy and both complete source endpoints, exact patch path list,
reverse application and forward roundtrip. This permits only this concrete
fix; it does not approve the main972 baseline or change runtime authority.

```sh
python3 tools/net-tcp-priority/test-selected-policy.py --selftest
python3 tools/net-tcp-priority/test-vector-policy.py
python3 tools/net-tcp-priority/test-selected-supplier.py
python3 tools/host-pthread-authority/minimum_receipt.py selftest
```

Existing negative controls remain, with additional receiver-choice, default,
rollback, metadata, coupled-header, patch/path and authority-pin mutations.
Independent review must assess the actual production delta and its baseline
couplings, not infer correctness from hash preservation alone.
