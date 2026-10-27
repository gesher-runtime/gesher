# Track brief: net4

Remaining network contracts after web3's current implementation and the
ADR 0009 audit. Reserved ADR: 0070. Branch `wip/net4` starts from integration;
merge verified web3 before implementing dependent changes in its own worktree.
Follow track-rules.md, CI-only AOSP builds, absolute paths and no WSL.

## Required work

- Fix the legacy resolver ABI: current gethostbyname/gethostbyaddr forward a
  Windows hostent (16-bit family/length) through a Linux hostent declaration
  (32-bit fields). Translate structures, addresses, aliases, lifetime and
  thread/error state, including reentrant variants and family selection.
  An own-file probe reproduces family 262146/length 0 instead of 2/4; this
  is not a missing-symbol-only problem.
- Implement the 84 libc resolver/network entries assigned to net4 in
  docs/handoff/native-api-coverage.json: netdb host/net/service/protocol
  databases, getifaddrs/freeifaddrs, resolver state/query/send/search/hooks,
  DNS wire/name helpers, interface/cache and inet APIs. Honor pinned bionic
  data layouts and error codes; never satisfy counts with dummy exports.
  The seven Android per-network exports already implemented by web3 retain
  that owner and require their own current CI evidence.
- Share socket O_NONBLOCK state across dup aliases and test alias closure,
  dup2 replacement and cross-process ownership with binder2. Implement the
  remaining ancillary SCM_RIGHTS/IP_PKTINFO, MSG_TRUNC and POLLPRI behavior.
- Close/document with executable evidence the AF_NETLINK/AF_PACKET/AF_VSOCK
  contracts called out by ADR 0009, pipe/socket receive-timeout behavior and
  unsupported-family errors. native3 owns generic epoll/poll semantics;
  binder2 owns binder/InputChannel cross-process descriptor transport.
- Verify default raw DNS on IPv6-only and dual-stack networks: web3 currently
  chooses an IPv4 default route, though explicit IPv6 network selection exists.
  Preserve resolver numeric-service/DNS failure distinctions and canonical
  names in selected-interface resolution.
- Complete Wi-Fi TransportInfo/SSID/signal details, VPN classification beyond
  adapter-name heuristics, actual captive-portal state transitions and the
  remaining connectivity hidden keepalive/QoS/agent contracts in ADR 0037.
  Respect Windows WLAN consent and report hardware coverage honestly.
- Audit the BoringSSL no-assembly limitation named in network.md: enable and
  validate the toolchain's supported accelerated paths with upstream crypto
  tests and an honest comparison; preserve TLS/certificate/error behavior.

## Domain and verification

Own compat/net, compat/posix/src/{netdb,socket,winsock}.cc and their neutral
headers, bionic networking wrappers, app connectivity glue and focused tests.
Coordinate fd.cc/socket_internal.h changes with binder2 and native3 before
editing; kernel-shared socket mode and per-process metadata differ.

Exercise real local IPv4/IPv6 peers/DNS fixtures and both x86-64/arm64 ELF
callers through dalvikvm/native processes/APKs, both execution modes and both
Windows runners. Assert actual interface/source addresses, wire results,
structures, callbacks, errors and lifetime. Keep local TLS, external UIA,
package install/run/uninstall and both required e2e jobs green.
Merge latest integration before final CI and read every claimed PASS line.

## Status and next steps

The separate legacy resolver candidate is now full-green40519019567 at
f36317637e01fdeeebcb1cbc4777c1cc8f9cea9a: both Windows0failed/730topPASS,
all16 real NDK contexts per runner, independent host/service oracle8 per
allowed standalone ABI, actual four-thread TLS/error/lifetime/permission
controls and cleanup. Package23 and both ten-app e2e sets0unexpected pass;
all successful logs and28job outcomes were read. [Bounded resolver evidence](net4-resolver-final.md).
These four pre-existing exports needed real ABI/result ownership correction,
not new symbol rows. Inventory bytes/counts/145 existing proofs and the82
remaining Net4 entries stay unchanged. Root alone accepts/merges this source.

The next bounded legacy resolver component starts in the separate
`wip/net4-resolver` worktree from accepted integrationbc6e05be; test82 and
existing ADR0070 are reserved. [Its brief](net4-resolver.md) records exact
Android/WinSDK ABI and lifetime/error semantics. Production source is prepared;
real NDK/oracle/negative/thread-lifetime fixtures and CI proof follow. The
accepted interface stage stays frozen. No additional entry/count is closed.

Coherent full **40499454478**, exact source
`694a4cd6b067f1deea87015bceb2ad2160260154`, attempt1, is complete green:
28jobs22success/6skip, both actual Windows suites0failed with712 top PASS
records and no top FAIL. Each retains38 Windows4/14 Web3/4 exit70/10 format71/
2 action74 PASS, alongside Net4's2 ABI dalvikvm/4 APK contexts/2 standalone
callers with entries9/host-oracle9. Strict AAudio tone/frame/capacity/zero-loss
controls, package23 and both complete ten-app e2e sets0unexpected pass.
The five critical actual logs were read; [frozen evidence](net4-ifaddrs-final.md)
lists exact jobs. The two per-entry metadata objects now cite this coherent
run/source; every unrelated entry and historical field/count remains intact.
Coordinator independent acceptance/merge remains separate; no more runtime
source changes or unchanged-code CI reruns follow from this result.

The coherent continuation now includes accepted Windows4/Web3 integration
6297e80a3bddd1903790406bfc306cac92150153 (clean merge4e8b41a6).
Shared networking/native2 merges preserve all seven Web3 bridges, both new
ifaddrs bridges/module assertions and Media4's real AMediaFormat_new probe.
The four AAudio result hashes still match exact78ee5e12. Source outside docs
differs from6297 only by the bounded Net4 slice/diagnostic workflow and that
already-reviewed AAudio slice; no AOT/FPSR/Surface continuation is imported.
One new complete full run is required for this changed combination, including
Windows4/Web3/70/71/74 plus real Net4 host-oracle9, strict AAudio controls,
package23 and both complete ten-app e2e sets. Frozen6f073f45 remains the
documentation-only bounded proof above source646f; do not move that ref.

Bounded interface candidate full40469857757 at exact646f9fb1e6b932317c089c829b80ec9d58ee50d0
is complete green:28jobs22success/6skip, both actual Windows suites0failed,
package23 and both ten-app e2e sets0unexpected. Actual both ABI dalvikvm,
four APK contexts and two standalone callers pass per runner. Each standalone
prints entries9/host-oracle9 and matches real independent Windows records.
The exact imported AAudio slice also passes strict750/1250Hz/44100-frame/
capacity44100/lost0/xruns0 checks in all four app contexts, plus the small-ring
loss/configured-ring zero-loss counterfactual. Old1153.3Hz causality remains
unproven. [Frozen evidence](net4-ifaddrs-final.md) identifies every critical job.

Only getifaddrs/freeifaddrs gain bounded per-entry metadata for that exact
source/run. Every existing field, baseline count and unrelated entry remains
unchanged. Coordinator acceptance/coherent integration verification follows:
the next candidate must include newly accepted Windows4/Web3 integration6297e80a.
Earlier diagnostic history below is retained, not current acceptance status.

Corrected narrow40467169771 at55a17156 is complete green. Actual Windows
jobs122065270702/122065270474 retain the nine old missing-index observations
and produce nine real AF_UNSPEC records each: physical, loopback and vEthernet
links plus their six IPv4/IPv6 addresses, prefixes, scopes and MACs. This proves
the Windows oracle collection only; both standalone ELF comparisons and full
acceptance still need a new full run.

The next candidate includes verified exit-freshness integration30deb08c and
the exact reviewed four-file AAudio diagnostic/lossless-fixture slice78ee5e12
(patch SHA256c38d41acbb60560b328d5c3ed06b8257b15c24c7d18cd69ad9893ff348e80179).
Each imported result hash matches its audit. It requests a real one-second
blocking-input buffer, requires actual capacity/frame/xrun/zero-loss telemetry,
and contrasts unread small/large rings fed by the same capture engine.
750/1250Hz,2% tolerance,44100 frames and the separate three-cycle overrun
assertions remain unchanged. The old1153.3Hz result had no loss counters and
is not claimed to have a proven overrun cause. Runtime proof remains pending.
The independently running coherent Windows4/Web3 source160e25e8 is untouched.

Narrow40466896274 at4904ecc3 reproduces the oracle error on both Windows:
nine active NDIS filter/switch interfaces have neither IPv4 nor IPv6 bound;
both .NET protocol-property calls throw10043. The actual physical, loopback
and vEthernet IP interfaces retain valid indexes. Microsoft documents that
the production GetAdaptersAddresses(AF_UNSPEC) call, without
GAA_FLAG_INCLUDE_ALL_INTERFACES, enumerates IP-bound interfaces. The corrected
shared oracle logs unbound NDIS interfaces and includes every active IP-bound
interface with strict index/address/prefix/scope/MAC checks. Its old raw
failure is retained in the diagnostic output, and host-oracle>=3 is unchanged.
This is an explicit oracle-domain correction; no production enumeration,
ABI/lifetime assertion or timeout is changed. Corrected narrow and full CI
are still required, including the separate reported Media3 failure.

Full40462148835 at719ae176 is red: both Windows test72 scripts abort
before the standalone caller because the .NET oracle did not obtain an
interface index. Actual dalvikvm ifaddrs checks pass for both ABIs on each
runner; APK aggregate checks also pass, but no host-oracle claim follows.
The2022 log separately contains a Media3 AAudio input-read assertion failure,
which is reported to that owner and remains visible. The next narrow CI
probe records every adapter, both protocol indexes and the exact exceptions,
then repeats the original index selection. No index source, interface coverage
or assertion is relaxed before the actual diagnostic output is understood.
Historical API inventory remains unchanged and acceptance is pending.

Corrected cache scope40461056132 at9bb847a8 is complete green; actual
framework122045549772 was read. All10 lifecycle controls pass, including
held Git with external cwd and both global path-option forms, plus successive
relative -C. Actual restored4fb cache has no locks, javac exits0/0errors and
the snapshot has no live Git/locks. Root's second bounded source review found
no additional actionable issue. The API/runtime stage is still unverified.
Current preparation includes latest integration53898b51 (mergecda052b0),
which adds only coordinator handoff docs above the existing runtime base.
The next complete full run must establish both zero-failure Windows suites,
all new ABI/process/oracle markers, actual package cleanup and both ten-app
e2e logs. All historical baseline counts remain unchanged until scoped
per-entry evidence is actually accepted.

This worktree is now assigned to the Windows4/Web3/release agent for the
getifaddrs/freeifaddrs slice only. Root's clean6519f82d merge of verified
integrationf17d03b1 is preserved; merge latest integration before CI. ADR0070
and test72 are used, while test71 belongs to Media4 and test73 to AOT3.

Prepared production changes add the two real System V exports and static LP64
structure/family guards over the existing GetAdaptersAddresses backend.
The public-entry-at-offset-zero allocation invariant is also asserted. Pinned
official ifaddrs.cpp matches the saved source byte-for-byte (SHA256
6416b09b239c2e93585ec57ebfb558a5cb634ad6bd0cf3d98f8429114b4da453), and
the public/packet headers were read directly from android-16.0.0_r4.

Native2 fixtures now include real list/loopback/IPv4/IPv6/mask/broadcast/
hardware/index/lifetime checks in both ABI dalvikvm and APK modes. A shared
NDK implementation also builds as standalone ELF in both ABIs; test72 compares
those results with a separate Windows .NET adapter snapshot. Media4's narrow
native2 availability edit is coordinated and is not overwritten. No app Java
or native2 app build script change is needed. Only PS AST, source/hash and
diff checks have run locally. All C/C++/Java/C# compilation and every executable
fixture are CI-only. Runtime/full CI and per-entry metadata remain pending;
none of the84 historical entries or other contracts is marked closed.

Full40456605869 at1f5a7e92 is superseded/cancelled once under Root's
instruction after peer source review found a guaranteed fixture invocation
error: gesher-exec --elf requires an explicit argv0 after the program path.
Without it the oracle path became argv0, so the main received no oracle and
the strict host-oracle>=3 assertion would fail. The next source adds the
program name before the oracle path; no production code, assertion or timeout
changes. The replaced run supplies no host-oracle/runtime acceptance. A new
complete corrected full run is required; do not reinterpret it as a retry of
unchanged code or relax the existing assertion.

Corrected full40456946272/fa8b29e2 builds both NDK ABIs, ELF binaries and
APKs successfully (actual122032381864 read), but framework122032814915
fails before javac on wificond/.git/info/sparse-checkout.lock. The exact cache
producer is the cancelled prior framework122031504583: cancellation05:44:43Z
was followed by always-save05:44:51Z under key suffixe831265ca95d55bb,
cacheID9564745920. The corrected run restored that exact key and later saved
the failed tree again asccc8acf1d1e8d4de/cacheID9564776068. Logs/cache API
metadata establish this chain; no local cache/artifact bytes were downloaded.
No runtime proof follows from the compiler-only NDK job.

Root assigned the infrastructure correction before another run. Framework
source snapshots now exclude cancelled jobs and reject live Git/remaining
locks. A Linux GitHub-Actions-only guard immediately after restore records
incoming lock paths/hashes and removes only recognized stale Git locks under
the exact AOSP workspace, before any source Git child. It refuses local or
external paths, symlinks, unknown locks and active Git; snapshot never deletes.
Lifecycle fixtures (including real held Git processes) run in CI before restore.
Only Python AST/diff inspection has run locally; the fixtures and corrected
full run still need CI. No unchanged retry or cache-key eviction hides the
incoming-lock evidence. Net4 runtime source and historical inventory stay fixed.

Scoped framework40459897967/a1c649b4 completes green; actual122041786365
was read. Seven initial lifecycle fixtures pass. Before any source Git command,
the exactccc8 cache restores zero-byte wificond index.lock and
info/sparse-checkout.lock with the interrupted producer's mtime; prepare logs
and removes both, javac completes with zero errors, and the snapshot has no
active Git/locks. This confirms the cancelled-cache chain, not a parallel
checkout failure. That scoped source still misses global Git path-option
liveness forms identified by Root review. The next source parses --git-dir/
--work-tree in equals/separate forms and successive relative -C bases; three
additional fixtures hold real Git commands outside the source cwd and isolate
those cases. Ten-fixture/corrected-source CI and full runtime proof remain
required. The earlier seven-fixture success does not prove the new controls.

The earlier fixture-only stage is merged through verified Media3 integration
f17d03b1: four local certificate rejections, NetApp HTTPS and deterministic
HTTP200/two-host SNI selection with isolated missing/unknown-name controls,
retaining real example/Google DNS and Google HTTPS204/TLS1.3/SNI/hostname.
Root personally read full40448521754/dd06d296, all28 jobs, both zero-failure
summaries, package and both ten-app e2e logs. This fixture verification supplies
no Net4 runtime/API closure or inventory rewrite.

Earlier Media3 full 40438975894/0148698 remains red only on 2025's five
external-network failures, despite all media gates passing; no timeout is
accepted as a certificate result. The first positive proposal's server
evidence could be polluted by an earlier localhost HTTP connection. Separate
HTTP and SNI listeners are required before acceptance. All contracts above,
including the 84 assigned libc/resolver entries, remain open. Every compiler
and native/JVM fixture runs in CI only.

Web3 full40444053066 atcbf5639 completes green (22 success/six skipped),
with all14 Web3 checks and zero failed tests on both runners, package and
both e2e logs read. Its next source additionally requires wrapper/child
exit codes zero; the earlier harness did not prove that after app done.
That stricter full validation remains required before final Web3 handoff.
No Net4 runtime/API contract is closed by the shared test-fixture preparation.
