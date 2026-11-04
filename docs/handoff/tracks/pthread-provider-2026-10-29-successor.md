# Pthread provider successor checkpoint, 2026-10-29

This is a handoff, not a release or integration acceptance. Manager explicitly
requests a high-reasoning successor; preserve sole original integrator's
ownership of integration/main/release. No native/JVM/C# compilation or fixture
execution locally; no WSL, CI artifact downloads, or access to the forbidden
backup directory. All GitHub writes use gh-authenticated transport after an
immediate `gh api user --jq .login` equals `palmoni5`; every commit includes
the required Claude Opus 5.5 trailer. Peer review is required before each
meaningful source/provenance change and before CI. One watcher per run only,
using the prescribed `gh run watch` interval. No watcher remains active.

## Sources and exact bounded evidence

* Clock tree `C:/work/gesher-pthread-provider-completion`, branch
  `wip/pthread-provider-completion`, source
  `1653cb92d2328fcd1bcefc2c967730b00d397511`, followed by this docs-only
  checkpoint. No implementation is qualified for integration. Prior preserved
  `C:/work/gesher-host-public-clock-dispatch` remains at `ff50d473` and was not
  edited. `40909289961/e0a89122` failed supplier compilation; fixed declaration
  order `1b3c34b9` produced `40910269651` diagnostic green but proved static
  and DLL clock IDs alias distinct actual threads. `40911784373/ace7a6ce`
  additionally proved both distinct pthread identities equal 1 and wrong-provider
  acquisition resolves to the local target.
* Clock-only anchor `2f7d8c519de7eef8321a030876bfd5c5250ad7b8` passed
  `40917856459` Linux and both Windows; independent actual audit is
  `C:/work/gesher-ci-review/pthread-clock-409178-independent-audit.md`.
  It uses a host executable export with a versioned descriptor and kernel
  object comparison, and pins the fallback DLL. It does not solve pthread_t.
  `79f11ebd756273c80949f08c79336929f965d3c1` passed `40919168634` on both
  Windows, including a malformed descriptor rejection with unchanged output and
  process handle count. This is one negative case, not full security or ABI.
* `40919845971/1653cb92` all three jobs passed: Linux single-provider direct/
  dlsym reference, plus Windows 2022 job123610659137 and 2025 job123610659287.
  Each observed `MIXED_PTHREAD static=1 dll=1` for distinct actual threads,
  and `MIXED_SAME_THREAD static=1 dll=2` for the same native thread; the
  latter's clock ID was deduplicated. Cross-provider clock reads matched
  separate kernel brackets; DLL unloaded while its main-thread record existed,
  and host-clock use and invalid-descriptor checks passed. This characterizes
  one path, not safe general DLL unload. Raw logs are
  `C:/work/gesher-ci-review/pthread-clock-409198-{compile,2022,2025}.log`.
  Independent actual audit requested from continuation_requirements_audit.
* Retirement tree `C:/work/gesher-pthread-retirement-provenance`, branch
  `wip/pthread-retirement-provenance`, frozen docs-only `efa5136b` over exact
  verified source `986b23949b26cadd0dbcea072b7c621b84cb13b1`, both pushed.
  Component40915541993 passed on both Windows: static/DLL original +8 bounds
  exactly, 338 destructors, 64 contention, outside-lock destructor, live join,
  256/reuse, self-join/tryjoin, foreign tryjoin, and static/DLL actual
  TLS-clear-failure abort sentinels. Independent actual audit and complete
  source/role limitations: `C:/work/gesher-ci-review/pthread-retirement-409155-independent-audit.md`.
  Init/TlsGetValue failure injection and additional race controls remain open.
  `40914415494/e9fb58ed` remains red due inventory contaminating a strict
  failure sentinel; `986b` suppresses inventory only in failure mode.
  `40912359945/06a525f6` actually observed the +10 controller's retained
  Thread and Event values matching supplier bookkeeping; 409110 established
  the Thread as live controller, not one of 338 fixture targets.
* Other preserved source trees: `C:/work/gesher-host-pthread-authority` at
  `5b322047`, `C:/work/gesher-pthread-external-exit` at `7bf53602`, and
  `C:/work/gesher-pthread-foreign-cpu-clock` at `8e6870d1`; do not alter them.
  Retirement and clock branches diverge at `5b322047`: neither includes the
  other's changes. Supplier.patch and fault-supplier.patch overlap materially;
  combine carefully with peer and fresh CI, never claim either receipt covers
  the resulting source. 986b's supplier.patch does not include ff50 clock.c.

## Contract, topology and next implementation

Read `pthread-process-authority-contract.md`, ADR0075, roadmap and
`host-pthread-authority-adoption-plan.md`. Product host executables are static
and self-contained; the separate bionic/System V shim owns guest ELF identity,
including x86_64 and arm64 ABI, and cannot be equated to host winpthreads.
The process host public static/DLL contract must preserve same-thread identity,
distinct-thread identity, foreign acquisition/error/errno, lifetime,
retirement and non-aliasing, with real Linux references and both Windows.
No fallback may silently treat missing authority as successful conformance.

Compare two implementation directions with an independent peer before writing
production: (1) executable-owned registry allocates canonical non-recycled
thread identities and holds native object capabilities with per-provider
registration counts, plus safe routing for all supported operations; (2) a
versioned descriptor forwards every required public pthread entry to one
process-lifetime executable owner, preserving one TLS/key/thread registry.
The first needs atomic claim/publication and rollback before creation returns,
correct same-thread rekey, cross-provider join/detach/cancel/equal and other
operations, lock order and provider lifetime. The second needs a complete
pinned public export/signature inventory, typed ABI-safe forwarding and
coverage of self-contained executable, DLL-only/missing-anchor and callbacks
whose code might unload. Never infer these properties from clock-only registry
tests. Peer specifically warned that local record maps and IDs can alias,
refs can outlive images, and a clock-only fix is not public pthread closure.

The host supplier prototype relies on the shared `.github/actions/setup-build`
private verified compiler extraction; arm64, webview and angle producers bypass
it and need explicit handling. Trace all host/translator producer link commands,
static/DLL imports and exact supplier receipts, no stale DLL-only swap, package
notices and install/uninstall. Keep bionic both ABIs and actual ART/APK, full
normal Windows, package and e2e gates. `5b` host wrapper/overlay and supplier
are themselves only bounded scoped evidence, not qualified product prerequisites.
Peer the exact combined source and provenance; source-only docs above a green
component do not replace a full combined run.
