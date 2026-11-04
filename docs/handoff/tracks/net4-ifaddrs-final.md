# Net4 interface-address candidate evidence

[Full40499454478](https://github.com/gesher-runtime/gesher/actions/runs/40499454478),
attempt1, source `694a4cd6b067f1deea87015bceb2ad2160260154`:
28jobs22success/6skip, both actual Windows suites `0 test(s) failed`.
This coherent source includes accepted Windows4/Web3 integration6297e80a.
The earlier bounded40469857757/646f proof remains frozen at6f073f45 on
wip/net4-ifaddrs-verified; that immutable reference is not moved.

| Actual job | ID | Evidence |
| --- | --- | --- |
| Windows2022 |122176176495|2 ABI dalvikvm,4 APK contexts,2 standalone callers;0 failed |
| Windows2025 |122176175888|Same contexts;0 failed |
| Package |122176175931|23 installed runtime/window/input/uninstall PASS |
| E2E2022 |122177141823|All ten apps,4 transport controls, association/shortcut,0 unexpected |
| E2E2025 |122177141801|Same complete set;0 unexpected |

The two standalone callers on each runner print:

> net4 ifaddrs: all passed; LP64; IPv4/IPv6 loopback; netmasks/broadcast; packet index/hardware; independent lists; entries9; host oracle9

Whitespace in that compact excerpt is normalized. Each actual oracle has
three link records plus six IPv4/IPv6 records from physical, loopback and
vEthernet interfaces. IPv6 scope, contiguous prefix/netmask, broadcast,
six-byte link hardware/type, index/name roundtrip, public layout, twelve
independent-list mutation/free cycles and a held unchanged list are checked.
freeifaddrs(nullptr) is exercised. Both dalvikvm libraries and all four real
ABI/APK process contexts also require the shared ifaddrs module to pass.

The earlier oracle error is retained: diagnostic40466896274 shows nine Up
NDIS-only interfaces per runner without IPv4/IPv6 binding; both property calls
throw10043. AF_UNSPEC without INCLUDE_ALL_INTERFACES excludes that domain.
Corrected40467169771 retains that counterexample and collects all actual
IP-bound records; the unchanged full host-oracle>=3 assertion receives9.

The imported four-file AAudio fixture/diagnostic slice78ee5e12 keeps the
750/1250Hz windows,2% tolerance and44100 required frames. Every app context
has requested/effective capacity44100,read44100,lost0,xruns0; retained captured
frames are44241/44269 on2022 and44361/44417 on2025. The controlled unread
3528-frame ring reports loss/overrun, while the44100-frame ring remains
lossless and both drain exact retained frames. This does not establish that
the earlier1153.3Hz failure was caused by overrun; that run had no counters.

Each runner also retains38 top Windows4,14 Web3,4 generation-aware exit70,
10 format71 and2 action74 PASS records; whole-suite top PASS count is712,
with no top FAIL. Those are log records, not API inventory counts. Framework
and both NDK callers build successfully. Package actual installed oatdump/image/dexopt,
Win32 rendering/input/resize and process/data/association/shortcut/toast cleanup
pass. Both e2e sets include Just Player/Puzzles/TIC80 with0 unexpected failures.
No CI artifacts/caches or local compilation/fixtures were used locally.

Per-entry verification is limited to getifaddrs/freeifaddrs in those actual
success contexts. Allocation failure, down/unbound/other physical adapters,
route/source selection, hostent/resolver databases and wire APIs, descriptor
alias/ancillary/unsupported-family and other Net4 brief contracts remain open.
Historical inventory observations/counts are preserved exactly; there is no
full84-entry or general networking closure. Only Root merges integration/main.
