# Virtual epoll registrations follow the shared file owner

This N33/T12 increment for [#56](https://github.com/gesher-runtime/gesher/issues/56)
repairs eventfd, timerfd, inotify and nested-epoll registration lifetime. It is
based on main `20252fde18db387b4d99d99f7050d6336f58bf6e`. The source is independently
written under the project license; kernel sources below establish contracts and
were not copied into the implementation.

## Application-visible contract

An epoll registration identifies both the number passed to `epoll_ctl` and its
open file description. Closing that number does not remove the registration
while another duplicated descriptor or genuine in-flight operation still owns
the same file. A different file reusing the number can be registered separately.
`MOD` and `DEL` must address the current file at the supplied number: a duplicate
at a different number cannot address the original registration, but restoring
the original number to the same file with `dup2` can.

The preceding implementation keyed virtual registrations by the number alone,
removed them on any close of that number, and sampled readiness through that
potentially recycled number. It also built a list of every live epoll and locked
each one on every virtual close, including closes of entirely unwatched files.

The pinned Android common kernel
[`fs/eventpoll.c`](https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2025-06_r38/fs/eventpoll.c)
defines `epoll_filefd`, fills/compares both members in `ep_set_ffd`/`ep_cmp_ffd`,
looks them up in `ep_find`, and retires the file's own registrations in
`eventpoll_release_file`. `do_epoll_ctl` holds current file references during
validation and rejects a nested self-alias by file identity. SHA-256:
`bef423c5c795a317e9492581212f161aa75d2080071fd4e9e05439819fb70dc8`.
In the same tag, [`fs/file_table.c`](https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2025-06_r38/fs/file_table.c)
runs `eventpoll_release` from final-reference `__fput`, and
[`fs/read_write.c`](https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2025-06_r38/fs/read_write.c)
holds its `fdget_pos` file through the actual `vfs_read` before `fdput_pos`.
Their hashes are recorded in the adjacent JSON report.
The same public calls run without adapters on native Linux in this change.

This preserves application inputs, permissions, requested operations and error
results. It does not supply fake readiness or replace a requested source. It
does not claim general socket/pipe/fence alias ownership, cross-process transfer,
`EPOLLEXCLUSIVE`, or new edge behavior for nested epolls. Readonly edge generation
is the separate [#293](https://github.com/gesher-runtime/gesher/pull/293) increment.

## Ownership and synchronization

The virtual registry stores one shared `ObjectOwner` per description, with its
current aliases and weak subscription list. Eventfd/timer/inotify lookups and
actual read/write operations alias that owner, so an admitted operation can
finish after the final public fd closes. Entries use a weak `(number, owner)`
key. Weak control-block comparison also distinguishes a later allocation at the
same address and does not keep the source alive.

Final owner destruction removes only that owner's still-matching entry ids.
It happens outside the registry lock. Registration locks epoll state before
updating the registry subscription list; registry close notifications only use
independent wake channels and never take parent state locks. Producers retain
the existing source-to-wake lock direction. Final cleanup validates both the key
and entry generation, so a concurrent `MOD`/`DEL` cannot remove a newer entry.
The generic nonvirtual close path and native-fence retirement remain unchanged.

Readiness is sampled from the captured owner, not a reused descriptor. Strong
sampling references are short-lived and are dropped outside epoll state locks.
No strong watched owner survives the actual blocking wait. This also requires
keeping virtual eventfd-backed targets out of generic `PollFds`: its ordinary
poll operation legitimately retains `LookupEventFd` while blocked. Epoll instead
subscribes its persistent waiter channels directly to the source for LT as well
as ET interests; disarming ONESHOT suppresses that subscription.

A nested epoll can use a surviving numeric alias for the existing mixed-poll
transport while still validating readiness through its captured owner. If all
of that target's aliases close but its own real in-flight operation still owns
it, the outer waiter samples with the existing 10 ms mixed-poll cadence because
there is no numeric target for generic pipe/socket descendants. The outer
observation does not become a persistent owner. This rare path is explicitly
bounded polling, not a claim of universal event-driven nesting.

## Production and differential verification

Run the maintained gate:

```sh
python3 tools/test_host_event_loop_contracts.py --sanitize
```

It retains the preceding fairness, vector dispatch, select, scratch-storage and
complete eventfd-edge corpora, and calls the new complete-source owner fixture:

```sh
python3 tools/test_epoll_virtual_owners.py --sanitize
python3 tools/test_epoll_virtual_owners.py --sanitize \
  --source-ref 20252fde18db387b4d99d99f7050d6336f58bf6e
```

The public Linux corpus passes **122 checks**: `dup`, `F_DUPFD`,
`F_DUPFD_CLOEXEC`, `dup2`, `dup3`, LT/ET/ONESHOT combinations, duplicate keys,
closed-fd errors, distinct reused files, restored original identity, nested
aliases/cycles, and real timerfd/inotify producers. The complete production
epoll host passes **535 checks** including the existing 153 checks. The exact
unchanged baseline compiles and runs the same fixture, failing **63 checks**.
The baseline result is an intentional negative control, not a passing gate.

The host compiles all of production `epoll.cc`. Win32 event handles, CRT numeric
reservations, generic `PollFds` and unrelated OS backends are declared adapters.
New descriptor adapters execute the actual registry retirement order used by
`fd.cc`; the native Windows probe separately executes the real syscalls. Host
timer/inotify payload objects are isolated lifetime controls, not emulated
backend success. Real Linux and the Windows probe exercise real timer/inotify.

The additional controls distinguish a paused or genuinely blocked epoll
observation from a real blocked `ReadEventObject`; test last-fd close with both;
retain a nested target only through an admitted child wait; test two readonly
payload lookup owners; and replay 32 controlled close/reuse/publication gaps.
Every run checks that all registry objects and native boundary handles retire.
Independent review identified and corrected an initial host-boundary weakness:
the new runner now retains `LookupEventFd` results for the entire `PollFds`
call, matching real `fd.cc`, rather than releasing them before waiting. An
explicit wake-to-return barrier makes the last-close assertion deterministic.
The maintained gate also compiles an isolated mutation feeding the numeric
virtual target into `PollFds` instead of `-1`. Its 26-case lifetime corpus must
fail exactly the watched-owner expiry assertion; production source is never
edited for that control. The corrected production passes while the mutation
produces exactly one expected failure.

ASan and UBSan pass. Only LeakSanitizer is disabled in the local executor with
`ASAN_OPTIONS=detect_leaks=0`, because its ptrace environment rejects LSan; the
repository's ordinary CI invocation does not disable it. The selected-policy
self-test still passes its original 20 negative controls without changing pins,
deadlines, supplier receipts or authority predicates.

Both x86-64 and ARM64 Windows production `epoll.cc` and `epoll_probe.cc` compile
with the existing warnings-as-errors flags and the unmodified, checksum-verified
LLVM-MinGW 20261006 publisher. This is cross-compilation evidence, not selected
supplier or Windows runtime evidence. The ordinary Windows `epoll_probe` gate
now requires both owner-corpus completion markers; `--virtual-owner` also runs
the focused real Windows case. Windows/ART execution remains to be observed.

## Measured work and costs

The fixture counts actual C++ allocations around the real production close
path. Setup is excluded from the close interval. Five alternating candidate and
unchanged-baseline runs used the same GCC 14.2 `-O1` build without sanitizers;
raw runs, hashes and limits are in the adjacent JSON report.

| Operation | Unchanged baseline | Shared owner | Interpretation |
| --- | ---: | ---: | --- |
| 4,096 unregistered virtual closes beside 256 live epolls: allocations | 36,864 | 0 | Removes the global census and its temporary vectors |
| Same close interval: allocated bytes | 33,488,896 | 0 | No temporary allocation for unrelated virtual closes |
| Same close interval: median time | 17.46 ms | 1.09 ms | About 16.0× faster in this host experiment |
| 10,000 zero-timeout waits on 64 ready registrations: median time | 614.64 ms | 628.78 ms | About 2.3% slower; an explicit common-path cost, not a Windows timing claim |
| Same ready-wait interval: allocations | 10,000 | 10,000 | One declared native-event adapter allocation per call; no extra warmed scratch allocation |
| Create 64 eventfds: allocation bytes | 18,872 | 28,088 | Additional 144 measured bytes per file for shared identity/aliases |
| Add 64 LT registrations: allocation bytes | 9,728 | 22,528 | Additional 200 measured bytes per registration for source/retirement subscriptions |

Construction allocations increase from 259 to 387 for the 64-file case;
registration allocations increase from 64 to 320. These persistent setup costs
buy the required lifetime and source-notification ownership. Counts include
declared event-handle adapters equally on both sides; they are not process RSS
or allocator-resident size estimates. Timing is supporting evidence from a
shared Linux executor, not a performance guarantee for applications on Windows.

Close work for a virtual source now follows its own subscriptions instead of
every epoll in the process; unwatched virtual closes have no census allocation.
The generic nonvirtual path still has its preceding census behavior. No broader
resource-efficiency claim is made.

## Composition and review status

Fresh overlap checks found that #73's current production scope is stdio/OFD
output, not this virtual registry, and #296's production scope is native-fence
SCM_RIGHTS receive flags. Their native-fence and generic descriptor paths remain
outside this change. The maintained host fixture has explicit ownership
adapters; its unchanged main20252 source control remains runnable. The audited
open heads were #73 `537936e89a450107821b0bcf4cd40cec0d060f24` and #296
`f6307ad47ba16d838770f100675e33f2eb84ed7a`.

An independent reviewer inspected the actual source, descriptor call paths,
locks, weak identity and final-reference cleanup, then replayed Linux122,
production535 and the exact 26/1 retained-poll mutation. The concrete initial
test-boundary finding was repaired. The review is scope compliant with no
remaining actionable blocker, bound to implementation commit
`89f31f6565fee83f72c3f3c6a44a1060b4e2d7f3`, tree
`9d5db1f89eec80cd6f8b7f22f02c2ff422060082`.

Separate local integration with #293's exact published tree
`e4c4a4e3a55a5a10b4db32ab155a7d4d9a7d8c58` produced commit
`80ea4d9914332acf38a73f0d5250b3d8cf1fb32e`, tree
`315044d316388de11926e05c23ffc7b5eccb8801`. Four overlapping files required
explicit resolution: epoll source, probe, Windows runner and maintained helper.
The resolution keeps readonly generation advancement, type-specific ET masks,
LT wake subscriptions and weak shared-owner identity, runs all three host
corpora, and requires all Windows probe completion markers. Each side's eight
disjoint files remains byte-identical; no unrelated path changed.

That combined source passed the maintained ASan/UBSan gate, including readonly
Linux137/production535, owner Linux122/production535 and the owner mutation,
alongside the existing fairness/vector/select/scratch/eventfd cases. Its original
selected-policy 20 negative controls, PowerShell runner parse and both Windows
target compilations also pass. Combined epoll SHA-256:
`d6838d452ec389a1dc3b19bc846e264c4c81ac8bc6aaaeb21e19ab96a27d3fc6`.
This is explicit integration evidence for a different tree, not a Windows
runtime pass or a claim that the independent PR already contains #293.
