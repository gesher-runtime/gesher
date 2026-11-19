# Updating Gesher from GitHub Releases

Open **Gesher Control Center** from the Start menu (or run
`gesher-app control-panel`). Its Updates card displays the installed version,
checks for a newer stable GitHub release, opens the release notes, and downloads
and starts that release's Windows installer.

Automatic checking is enabled by default and can be disabled in the card.
The panel checks when opened, at most once per 24 hours;
**Check for updates** performs an immediate manual check. No service, scheduled
task or startup program is installed. Checking and downloading use a background
worker so the dashboard remains responsive. Closing the panel or cancelling a
transfer cancels its network work.

Installing requires confirmation because Setup stops the running Gesher device
and its apps. The normal interactive installer upgrades the current installation
in place, with automatic Windows restart disabled. The panel distinguishes
installer cancellation/failure from completion and reads the installed version
again before reporting success. Save work in open Android apps before proceeding.

Installed Android apps, their data, accounts and settings remain in
`%LOCALAPPDATA%\Gesher`. The updater runs the new installer over the current
installation; it never runs the uninstaller. Uninstalling Gesher has different
semantics and deliberately removes that profile.

## Repository map for the update path

| Component | Responsibility |
|---|---|
| `compat/shell/gesher_launch.cc` | Native Start menu entry point; starts the packaged PowerShell launcher without a console and releases its own executable during an ordinary dashboard launch |
| `tools/package/gesher-app.ps1` | Routes `control-panel` before starting ART; also provides the existing device shutdown command |
| `tools/package/gesher-control-panel.ps1` | WPF update card, background worker lifetime, confirmation and installer completion |
| `tools/package/gesher-update.ps1` | Installed version, update preferences, release selection, HTTPS transfer and installer validation/handoff |
| `tools/package/gesher.iss` | Per-user Inno Setup installation, version metadata, device shutdown and replacement of a loaded shell extension |
| `tools/package/gesher-runtime-files.ps1` | Refreshes runtime-owned profile copies after an upgrade, including equal-size resources |
| `tools/i18n/messages.json` | English/Hebrew update strings; generated consumers remain synchronized by `tools/i18n/generate.py` |
| `.github/workflows/ci.yml` | Windows PowerShell contracts, installer version selection and real in-place upgrade checks |

The Android runtime (`compat/`), Java application/framework services (`app/`)
and ARM64 translation keep their existing installation layout. Updates replace
the complete runtime with the release installer, so binaries, boot images and
framework files come from the same package. Maintainers must publish the installer
that passed CI; the updater does not inspect CI attestations. This feature does not apply APK
updates or update individual DLLs independently.

## Release contract

The updater reads the public
[`releases/latest` endpoint](https://docs.github.com/en/rest/releases/releases#get-the-latest-release)
for **`gesher-runtime/gesher`**. It accepts only stable, non-draft releases and
compares numeric versions. A release tag may be `v0.2.0` or `0.2.0` (exactly
three components); the matching asset must be exactly `gesher-setup-0.2.0.exe`.

An installed Gesher has either a release version (`0.2.0`) or a CI build version
(`0.2.0.526`: the highest release tag, then the CI run number). A CI build is
ordered above the release it follows and below the next one, so a CI build of
0.2.0 is offered 0.2.1 but never 0.2.0. Before the first release tag, CI builds
are `0.0.0.<run number>`.

The asset must have GitHub's
[`digest` metadata](https://docs.github.com/en/rest/releases/assets), formatted as
`sha256:` followed by 64 hexadecimal digits. The download's byte length,
SHA-256 and Windows product version must match the selected release before the
installer can start. An installer with missing verification metadata is not
offered as an executable update. A checksum is an integrity check, not an
Authenticode signature or a promise that an arbitrary release is bug-free.

Setup writes `gesher-version.ini` under the installation directory, using the
same `AppVersion` as its Windows version information. A development checkout or
installation with absent/invalid metadata is shown as an unknown version; the
updater does not invent a version or automatically downgrade it. An already
installed equal or newer version is left alone.

There is currently one Windows package: x64, matching the installer's
`x64compatible` requirement. The bundled Android ARM64 translator is part of
that package; it is not a separate Windows ARM64 installer.

### Publishing a release

A release is published by pushing a tag. Choose a version newer than the latest
published release, tag the commit on `main`, and push the tag. The first release
must be at least `v0.2.0`: CI builds made before tag-based releases were
installed as `0.1.<run number>`, and a lower release would never be offered to
those machines. The workflow rejects a lower tag.

```sh
git fetch origin main
git tag -a v0.2.1 -m "Gesher 0.2.1" origin/main   # or a specific commit on main
git push origin v0.2.1
```

The tag starts a **ci** run of its own (`refs/tags/v0.2.1`). Its concurrency
group belongs to that tag only and is never cancelled in progress, so pushes,
merges and pull request runs cannot cancel it, and it does not cancel them. That
run builds the installer as exactly `0.2.1` and runs every production job,
including the package, upgrade and E2E checks. Only when `ci-required` succeeds
does the `release` job:

1. require a `vX.Y.Z` tag that still names the tested commit, that commit to be
   on `main`, and the version to be newer than every published release (a
   malformed tag such as `v1.2.3-rc1` already stops the run in its first job);
2. take the tested `gesher-setup-0.2.1.exe` from the same run and check it
   against its `SHA256SUMS.txt`;
3. create a draft release with both files and generated release notes, wait for
   GitHub's asset digest to equal the tested SHA-256, recheck that no newer
   release appeared meanwhile, and only then publish it as the latest release.
   Publishing is serialized across tags, so two releases cannot interleave.

If any job fails, nothing is published. Fix the problem on `main`, then delete
the tag (`git push origin :refs/tags/v0.2.1`) and tag the fixed commit, or use
the next version. A re-run of a failed `release` job replaces a draft left by an
earlier attempt; it never overwrites or downgrades a published release.

Branch, pull request and manual (`workflow_dispatch`) runs never publish, even a
manual run on a tag ref. Their
installers carry a CI build version and are not offered by the updater.

Until a suitable
release exists, a successful check reports that no release is available.

## Failure handling and resource use

Network requests use Windows/.NET HTTPS, the default proxy and normal certificate
validation, including trusted roots installed for a content filter or corporate
proxy. Windows PowerShell 5.1 can inherit a legacy-only TLS selection from its
older Framework target; the updater upgrades that selection to TLS 1.2. An
existing OS-default or explicit modern TLS selection is retained. This setting
affects future Framework connections in the panel process, without changing
Windows configuration. There is no custom CA bundle or certificate-validation
override. GitHub rate
limits, offline access, malformed metadata and transfer errors remain retryable;
they do not stop the running Android device.

Requests have finite deadlines. Installer data streams to a unique temporary
directory rather than being buffered as a complete byte array. Incomplete or
unverified downloads are removed and are never handed to Setup. Automatic
attempts are throttled even after failure; manual checks remain available.
Preferences are written separately from the runtime files and survive an upgrade.

Runtime-owned files copied into the Android profile used to be refreshed only
when their length changed. The launcher now records a completed installation
generation separately for the app and dex entry points. A new installed version
checks all relevant runtime files even when size and timestamp match. Identical
bytes are retained so another launcher can continue using a mapped ICU file. A
failed copy leaves the generation pending for retry. Normal launches use inexpensive
metadata checks; unversioned developer builds additionally compare content where
metadata alone cannot establish identity. App data is outside this operation.

## Verification

`tests/windows/shell/UpdateChecks.ps1` exercises the production core with
deterministic release/download/process boundaries. `ControlPanelUpdateChecks.ps1`
exercises the panel's update state and worker lifecycle. `RuntimeFileChecks.ps1`
uses real temporary files to cover equal-size upgrades, unchanged launches,
separate entry-point generations, retry and preservation of app data.

`UpdateTransportChecks.ps1` tests HTTP responses and redirects with in-memory
fixtures. On Windows it also starts from a legacy TLS selection and makes a
bounded HTTPS request through the production client to the fixed GitHub API.
Any HTTP response proves negotiation, so the check does not require a published
release and tolerates API rate-limit responses. Certificate validation remains
enabled for this real request.

The early `update-contracts` CI matrix runs Windows PowerShell 5.1 on Windows
2022 and 2025. The existing package checks also verify that the installed updater
and version metadata are present. The real installer upgrade test preserves app
data and update settings while an unrelated process holds the old Explorer shell
extension open. Existing full Windows/framework/E2E and source/artifact identity
gates remain enabled.

Portable PowerShell on Linux can exercise core behavior and parse the scripts;
it does not establish Windows PowerShell, WPF or Inno Setup behavior. Those require
the Windows jobs. A release that has not yet been published cannot be tested as
an actual public end-to-end download; offline fixtures cover its selection and
transfer contracts until such a release exists.
