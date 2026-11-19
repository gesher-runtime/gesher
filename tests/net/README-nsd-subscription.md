# Native service-info subscription checks

`WindowsNsdResolver` uses a separate subscriber mode for continuing service-info
observations. The original resolver and advertiser JNI tables, interface-array
shapes and native progress semantics remain separate.

## Native boundary

The subscriber snapshot has seven values per real adapter: canonical index,
IPv4 index, IPv6 index, LUID, packet generation, available-family mask and an
observed lifetime. The last value identifies request-owned continuity, not an
Android Network. Java retains its real Windows network mapping.

The lifetime remains stable across unrelated notifications, ordinary metric or
parameter changes, and a family removal while another usable family remains.
Observed whole-scope departure, last-address departure, replacement LUID or
changed mapped identity prevents reuse of the old service state. A snapshot
whose address/family observations are not reconciled returns a retry indication;
it is never converted into an empty network list by a timeout or retry count.
Unknown observations and resource exhaustion remain explicit unavailability.

The Windows notification contracts specify that callback rows are incomplete.
The interface callback uses only `Family`, `InterfaceLuid` and `InterfaceIndex`
as identifiers. The address callback similarly uses `Address`, `InterfaceLuid`
and `InterfaceIndex`. Positive operational and address observations come from
complete IP Helper queries. `Connected`, DAD state and other partial-row fields
are not accepted as evidence. Initial notifications do not indicate departure.

Primary platform contracts:

- [NotifyIpInterfaceChange](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-notifyipinterfacechange)
- [NotifyUnicastIpAddressChange](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-notifyunicastipaddresschange)
- [GetIfEntry2](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-getifentry2)
- [GetIpInterfaceEntry](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-getipinterfaceentry)
- [GetUnicastIpAddressEntry](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-getunicastipaddressentry)
- [CancelMibChangeNotify2](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-cancelmibchangenotify2)

Callbacks do no JNI or socket I/O. Their in-flight fence begins before taking the
state mutex, so snapshot capture sees callbacks waiting for that mutex as well
as callbacks already inside it. Generation is checked before and after complete
adapter capture. The owner cancels both watches outside every callback-needed
lock. A failed cancellation retains its exact handle for a later close attempt.

A subscriber send succeeds only after the complete datagram was sent on every
family of the selected configured adapter. Missing sockets, partial family
success, a stale generation or a retiring owner do not report full success.
The subscriber diagnostics remain six fields: live requests, live sockets,
notification registrations, in-flight callbacks, requests that completed a
poll, and actual successfully sent datagrams belonging to live requests.

## Finite host observations

Run from the repository root:

```sh
python3 tests/net/test_nsd_subscription_native.py --output-dir /tmp/nsd-subscription-host
```

The C++ fixture invokes the same production lifetime/fence/retirement helpers
with finite ordinary observations. It covers initial baselines, unrelated
notifications, family and address handovers, departure/return between polls,
identity changes, unresolved snapshots, bounded capacity and owned cancellation.
It does not modify network interfaces or send packets. `--sanitize` adds address
and undefined-behavior instrumentation. The receipt records `ASAN_OPTIONS` so an
environment where leak checking is unavailable cannot silently claim that check.

## Windows compile and link

```sh
python3 tests/net/test_nsd_subscription_native.py \
  --output-dir /tmp/nsd-subscription-cross \
  --llvm-mingw /path/to/pinned/llvm-mingw \
  --aosp-dir /path/to/patched/pinned/aosp
```

`--support-aosp-dir` may identify a separate copy of the same pinned bionic/ICU
build inputs. No dependency is downloaded or replaced by the runner. It rebuilds
the real source list from `compat/posix/posix.cmake`, the generated timezone
table, the full native transport and the existing `nsd_windows_link_test.cc`
fixture for x64 and ARM64. The fixture retains the genuine guest `bind`,
`sendto`, `setsockopt` and `select` entries alongside the Windows transport.
Compiler dependency hashes and the linked PE/archive hashes are saved in the
receipt. Direct guest-colliding Winsock imports remain forbidden in the native
transport object.

These are compile/link and host-helper checks. They do not execute Windows,
establish actual IP Helper notification ordering, prove native multicast
delivery, or qualify the installed-app callback and retirement gates. Those
Windows/runtime checks remain separately required.
