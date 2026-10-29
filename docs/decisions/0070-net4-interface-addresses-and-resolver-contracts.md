# 0070. Network interface addresses and remaining resolver contracts

Status: the bounded interface stage40499454478/694 was independently accepted
and merged by Root asbc6e05be. The next bounded legacy host/service correction
is verified by full40519019567/f363, both actual Windows0failed, and awaits
Root independent acceptance. [Resolver evidence](../handoff/tracks/net4-resolver-final.md).
Only the two interface entries have inventory verification metadata; the four
legacy symbols already existed. Historical counts,145 proof objects and82
remaining Net4 entries are unchanged. [Interface evidence](../handoff/tracks/net4-ifaddrs-final.md).

## Contract and implementation

The pinned android-16.0.0_r4 bionic ifaddrs header has a 56-byte LP64 object:
next/name/flags/address/netmask/union/data offsets are0/8/16/24/32/40/48.
sockaddr_ll is20 bytes, with signed interface index at4 and hardware bytes
at12. IPv6 family is10 and packet family17. Windows Winsock constants or
host structures must never escape through these declarations.

Gesher's existing GetAdaptersAddresses backend in posix/src/netdb.cc already
constructs those neutral Linux objects. Each entry owns a complete allocation
with embedded name/address/netmask/broadcast storage; freeifaddrs walks and
frees those allocations. The public entry must remain at offset zero.
Two missing bionic symbol-table entries now forward getifaddrs/freeifaddrs
through explicit System V bridges. Compile-time guards check public size,
alignment and field offsets, IPv4/IPv6 and packet layouts and family values.
No dummy success list, Windows-layout cast or alternate ownership scheme is
introduced. ifa_data is unused as in the pinned public bionic contract.

Pinned source audit and SHA256 values are saved in
C:/work/gesher-ci-review/net4-ifaddrs-pinned-source.json. Primary sources:
[bionic header](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/include/ifaddrs.h),
[implementation](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/ifaddrs.cpp),
[packet structure](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/kernel/uapi/linux/if_packet.h).

## Verification boundary

The same NDK-compiled checks run through native2's x86-64/arm64 dalvikvm
libraries, both ABI APKs in single-process/device modes, and standalone ELF.
They require actual127.0.0.1/8 and::1/128, contiguous masks, IPv4 broadcasts,
Linux link indices/hardware types, real six-byte Ethernet addresses and
name/index round trips. Held lists must survive twelve separate enumerations,
mutation and free of another list, with disjoint entries and names.

44-native2 requires the new ifaddrs part and process exit zero; each APK's
existing all-passed verdict also requires that part. Test72 runs both ELF
ABIs against an independent .NET Windows snapshot of active adapters, MACs,
addresses, prefix lengths and IPv6 scopes. It does not alter adapter settings.
The oracle's domain is active IPv4/IPv6-bound adapters, matching the backend's
AF_UNSPEC query without GAA_FLAG_INCLUDE_ALL_INTERFACES. Narrow CI40466896274
records why the original oracle failed: nine Up NDIS-only filter/switch
interfaces per runner have neither IP protocol bound and both property calls
throw10043. Corrected40467169771 preserves that counterexample and collects
all nine actual link/IP records on both Windows versions. Unbound interfaces
are logged explicitly; every IP-bound index and address assertion remains
strict. This oracle collection proof does not establish ELF/API acceptance.
Builds and every fixture execution run only in CI. Local PS AST/source/hash
checks are not runtime evidence. Both complete Windows suites, actual package
window/input/uninstall cleanup and both complete pinned e2e sets remain
mandatory before accepting the source.

## Remaining scope

The separate legacy component replaces raw Windows host/service result
pointers with deeply owned neutral copies and Linux-layout thread-local
publication, with explicit resolver error conversion. Full40519019567/f363
supplies test82's16 actual NDK contexts per runner, independent WinSDK data,
four-thread ownership/error races, permission and negative controls.
[The component brief](../handoff/tracks/net4-resolver.md) records exact pinned
semantics and explicit uninduced provider/OOM cases. Baseline metadata and
the accepted interface stage are unchanged; this is no full resolver parity.

The 84-entry historical Net4 export baseline remains unchanged. These two
entries may gain scoped verification metadata only after actual CI evidence;
the aggregate historical counts must not be rewritten. Broader host/family/
reentrant contracts, resolver/query/name-wire APIs, shared nonblocking ownership, ancillary data,
unsupported socket families, IPv6-only default DNS, connectivity/hardware
details and accelerated crypto remain the separate contracts in the Net4
brief. This slice establishes no AF_PACKET socket implementation or physical
network hardware coverage beyond the actual hosted interfaces inspected.

## Ordinary UDP receive truncation component (full acceptance pending)

Component40798611488/source8f224ecd actually passes both ELF ABIs on Windows
2022 and2025 against independently executed Linux192 normal/176 fault rows.
One complete Winsock datagram receive precedes fault-safe page-wise scatter,
so consuming reads retain no tail and MSG_PEEK retains the original packet.
Input MSG_TRUNC returns original length; recvmsg output flags report truncation
independently of that input. Linux-measured payload/metadata EFAULT and partial
prefix/queue ordering are required. OOB and WAITALL are ignored on ordinary
IP UDP receives; ancillary data and unrelated socket families remain outside
this contract. [Exact evidence and full gates](../handoff/tracks/net-udp-truncation.md).

No inventory/history metadata or aggregate82-entry closure follows from this
component. Full normal Windows suites, installed package and both e2e remain
required before integration acceptance.
