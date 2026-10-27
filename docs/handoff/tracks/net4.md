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

Reserved on 2026-10-26. No successor implementation or CI yet. Web3's latest
complete diagnostic is 40376044459 at
939ec313d0fe5a2d8410ee700a927fad0538269e: all 14 Web3 checks pass on both
runners, package and both required e2e jobs pass, but Windows 2022's boot-profile
performance failure keeps the full stage red. The old 40333695614 run was
superseded and supplies no complete runtime result. Web3 preparation includes
verified runtime/exec3 integration 94ccde1. Current changed-source full 40432795403
at 2755f21 adds the coordinator's verified startup profile merge 2ba9a11 and is
running with unchanged thresholds. Verified Web3 must merge before
dependent Net4 implementation in its own worktree. Every contract above remains
open; the coordinator's startup diagnosis does not permit fishing for a green
repeat or treating scoped passing checks as full-stage acceptance.
