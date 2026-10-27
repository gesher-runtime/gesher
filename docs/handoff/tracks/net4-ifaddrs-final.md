# Net4 interface-address candidate evidence

[Full40469857757](https://github.com/gesher-runtime/gesher/actions/runs/40469857757),
attempt1, source `646f9fb1e6b932317c089c829b80ec9d58ee50d0`:
28jobs22success/6skip, both actual Windows suites `0 test(s) failed`.
This bounded source predates the accepted Windows4 combination; the next
coherent candidate must include integration6297e80a before integration acceptance.

| Actual job | ID | Evidence |
| --- | --- | --- |
| Windows2022 |122076740538|2 ABI dalvikvm,4 APK contexts,2 standalone callers;0 failed |
| Windows2025 |122076740580|Same contexts;0 failed |
| Package |122076740561|23 installed runtime/window/input/uninstall PASS |
| E2E2022 |122077545268|All ten apps,4 transport controls, association/shortcut,0 unexpected |
| E2E2025 |122077545241|Same complete set;0 unexpected |

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
frames are44331/44687 on2022 and44333/44327 on2025. The controlled unread
3528-frame ring reports loss/overrun, while the44100-frame ring remains
lossless and both drain exact retained frames. This does not establish that
the earlier1153.3Hz failure was caused by overrun; that run had no counters.

Main122074087624 reports203 ABI boundaries/0problems; framework and both NDK
callers build successfully. Package actual installed oatdump/image/dexopt,
Win32 rendering/input/resize and process/data/association/shortcut/toast cleanup
pass. Both e2e sets include Just Player/Puzzles/TIC80 with0 unexpected failures.
No CI artifacts/caches or local compilation/fixtures were used locally.

Per-entry verification is limited to getifaddrs/freeifaddrs in those actual
success contexts. Allocation failure, down/unbound/other physical adapters,
route/source selection, hostent/resolver databases and wire APIs, descriptor
alias/ancillary/unsupported-family and other Net4 brief contracts remain open.
Historical inventory observations/counts are preserved exactly; there is no
full84-entry or general networking closure. Only Root merges integration/main.
