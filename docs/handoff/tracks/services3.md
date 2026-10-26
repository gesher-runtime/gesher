# Track brief: services3

Remaining app/service/device contracts in earlier service ADRs and status
sections. Reserved ADR: 0069. Branch `wip/services3` starts from integration
after aot2; use its own worktree under C:\work. Follow track-rules.md.

Read Services.java and each service implementation, docs/services.md,
docs/devices.md, docs/lifecycle.md and ADRs 0012/0019/0024/0025/0026/0032/
0034/0038/0043/0046/0051. Older limits may have been implemented later:
establish the current behavior before changing it or closing a gap.

Required audit and implementation, each backed by CI evidence:
- Remaining permission/exported/provider startup, started-service process
  lifetime, task launch-mode and settings-provider limits named in ADRs
  0025/0026 and their status docs. Compare caller identity and authorization
  with the current implemented permission gates, rather than assuming all
  services are still early Default stand-ins.
- Remaining default/zero-answer service paths named by the status docs and
  ADRs; audit real mount/storage, appops/appwidget/search and feature/SDK
  declarations against behavior used by apps. Record each concrete missing
  contract and implement it; do not claim full service coverage from an
  expected service-name list alone.
- App standby/doze, exact-alarm permission and app clock/time-zone requests
  in ADR 0038. Verify Android permission/failure contracts against Windows'
  actual authority model, including rejected requests and state changes.
- Remaining device APIs in ADR 0043: geocoding, GNSS details, geofences,
  sensor direct channels, camera reprocessing/offline sessions/extensions/
  torch and cross-process camera ownership. Keep advertised capabilities
  consistent with implementation and actual Windows hardware availability.
- Resolve the geocoder's potentially 40-minute Windows2025 wait at its
  underlying timeout/cancellation boundary; do not hide a hang with a retry.
- Read docs/e2e.md/ADR 0034's known limits and reproduce any still-applicable
  real-app/service failures. Pinned app smoke coverage passes in ci
  40320549410 but does not establish all public service contracts.

Coordinate camera/codec recording with media4, GL preview/hardware buffers
with surfaces2, locale state with locale2, network service details with web3,
and binder protocol with binder2. Existing toast/content/job/time-zone/
legacy-camera/preview work in ADR 0051 is verified and remains regression
coverage, not a reason to repeat it.

Use focused service and real-app checks on both Windows runners in both
execution modes. Preserve installer install/run/uninstall and both e2e jobs.
Merge latest integration before final CI. No local AOSP build or WSL.

## Status and next steps

Reserved on 2026-10-26 during the post-aot2 gap audit. Unstarted; no new
implementation or CI. The audit must separate superseded historical limits
with cited evidence from still-open contracts, retaining every unresolved
item until its own verification exists.
