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

Reserved on 2026-10-26 during the post-aot2 gap audit. Unstarted; all required
work above remains open apart from the explicitly cited windows3 relaunch.
