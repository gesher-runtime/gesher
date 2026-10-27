# Net4 legacy host and service resolver continuation

Worktree `C:\work\gesher-net4-resolver`, branch `wip/net4-resolver`, starts
from accepted integration `bc6e05bea341d1dc79920deaf3f069ad690fedc4`.
ADR 0070 and test 82 belong to this component. The accepted interface stage
and its immutable `wip/net4-coherent-verified` ref remain unchanged.

## Status and next steps

The initial native2 workflow dispatch returned404 because that workflow is
not present on the default branch; it created no run. A narrowly scoped push
trigger for this branch and these fixture/build files starts NDK compilation
without changing main. Production llvm-mingw and actual Windows execution
still require their own successful CI evidence.

Production source is prepared, without compilation or executable evidence.
Four existing legacy bridges now reach actual Winsock lookups through a
neutral boundary that copies every name, alias and address into owned bytes.
Linux-layout host/service objects have separate thread-local lifetimes;
Windows short fields and Win64 service-field ordering never escape.
Allocation checks use C allocation because the project disables exceptions.
Resolver failures publish the calling thread's real `h_errno`, keeping
internal Linux `errno` separate. Service calls do not change `h_errno`.

Pinned bionic numeric-name validation rejects shorthand IPv4 forms, while
ordinary names use the actual host resolver. Reverse lookups validate family
and exact address size, handle mapped/compatible IPv4 and reject scoped local
IPv6 as the pinned implementation does. Non-numeric/reverse lookups respect
the existing INTERNET permission; numeric conversion and local service
database reads remain available. Service lookup retains network byte order,
canonical-name matching and optional protocol selection.

Prepared fixtures build separate NDK JNI/standalone callers for both ABIs.
Four dedicated APKs cover each ABI with/without INTERNET in both app modes;
Native2's existing manifest and modules are unchanged. Actual numeric/localhost/
IPv4 and IPv6 reverse records, network-order ports including 443, aliases,
mutable copied storage, strict numeric/family/length errors, permission denial
and four concurrent native threads are required. Test82 runs eight caller
contexts per ABI, plus a separate bounded WinSDK oracle process that decodes
the Windows short fields directly and compares eight real host/service rows.
It requires fresh device app exit zero and actual uninstall/code cleanup.

The existing native2 workflow gains additive build/copy/symbol inputs only;
all original NDK libraries and app checks remain. Source PS AST/XML and Git
whitespace checks pass locally. NDK compilation, production llvm-mingw/Werror
and every executable fixture remain CI-only and pending. No API verification
metadata or baseline counts change before complete actual CI evidence.
Reentrant APIs, `gethostbyname2`, resolver wire/query/state functions,
per-network binding policy, database enumeration and broader network contracts
remain open.

## Pinned sources

Android sources all use `android-16.0.0_r4`:

- [Public netdb ABI](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/include/netdb.h):
  host/service fields are 32-bit; `h_errno` has its own thread-local address.
- [Bionic host lookup](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/dns/net/gethnamaddr.c):
  shared per-thread host storage, numeric validation, family/length errors,
  mapped/compatible addresses and scoped-IPv6 rejection.
- [Bionic service lookup](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/dns/net/getservent.c):
  separate per-thread service storage, canonical-name/protocol matching and
  network-order ports. Gesher uses the Windows service database contents.
- [Microsoft WinSDK header](https://github.com/microsoft/win32metadata/blob/6d6617a8eee57d7c4f6c55d8c23e35064ea69b9e/generation/WinSDK/RecompiledIdlHeaders/um/WinSock2.h):
  host fields at offsets 16/18 are 16-bit; Win64 service protocol/port fields
  are at offsets 16/24. Compile-time guards cover both sides.

Saved exact source digests are in
`C:/work/gesher-ci-review/net4-legacy-resolver-pinned-sources.json`.
These source checks establish no runtime acceptance or wider resolver parity.
