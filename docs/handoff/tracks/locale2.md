# Track brief: locale2

Remaining locale contracts after ADR 0049 and windows3. Reserved ADR: 0066.
Branch `wip/locale2` starts from integration after aot2; create a dedicated
worktree under C:\work when implementation begins. Follow track-rules.md.

Read docs/locale.md, ADRs 0044/0049/0053, LocaleService, SystemLocale,
Installer, ActivityTaskManagerService and tests/windows/tracks/49-locale.ps1.
The single-process configuration relaunch was already implemented by windows3
and verified in ci 40285476400; do not reimplement or count it as still absent.

Required work:
- Persist LocaleConfig overrides, with restart, replacement and clearing tests.
- Deliver ACTION_APPLICATION_LOCALE_CHANGED to the authorized installer and
  other recipients required by the pinned framework, including permission and
  package/user boundaries. The lack of an installer app is not verification.
- Remove package locale state on uninstall according to Android's retain-data
  semantics; test uninstall/reinstall and data-preserving removal separately.
- Verify Windows display-language list changes through the real NLS access
  boundary and event path. Distinguish a controlled source test from an actual
  Windows sign-in; retain the latter as unverified until there is evidence.
- Audit the explicit custom decimal/grouping/date/time/currency and differing
  regional/display-language limits in ADRs 0044/0049. Match Android's public
  configuration/formatting contracts and document concrete behavior/tests;
  do not silently delete an owner's known gap as a design exception.
- Own locale/time-zone limits routed from native3's POSIX audit. Coordinate
  bionic wchar/Unicode data and ABI details with native3 rather than editing
  its shared files concurrently.

Use a separate available track test number and focused Java apps. Check actual
resource/default-locale/format results and callback ordering in single-process
and device modes on both Windows runners; package and both e2e jobs must pass.
Merge current integration before final CI. No local AOSP build or WSL.

## Status and next steps

First bounded slice started on 2026-10-27 in `C:\work\gesher-locale2`,
`wip/locale2`, from accepted integration `7b49a718`. ADR0066 and test81 cover
persisted override/replacement/clear, selected-locale reconciliation and ordinary
caller/package/user boundaries. Source and actual APK fixtures are prepared;
compilation/runtime/full acceptance are pending. See
[ADR0066](../../decisions/0066-locale2-persisted-config.md) and the
[pinned-source audit](locale2-source-audit.json).

Privileged grants/installer/IME identity, permission-gated broadcasts,
uninstall/retain-data, Windows NLS/regional-format and native locale/time-zone
contracts remain open. The all-packages-same-UID, PM all-packages UID query and
AMS all-granted permission stand-ins are concrete Services3 gaps, not authority
for this slice. The frozen Native/FPSR worktree and its active full CI are
unchanged. No local compilation or native/JVM/C# fixture execution.
