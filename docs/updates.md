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
(`0.2.0.526`, with the highest usable published stable version followed by the
CI run number). A CI build is
ordered above the release it follows and below the next one, so a CI build of
0.2.0 is offered 0.2.1 but never 0.2.0. Before the first usable published
release, CI builds are `0.0.0.<run number>`. A bare Git tag, draft,
prerelease, or release missing the updater's required installer metadata does
not establish a new baseline.

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

Push a version tag to the official repository to start a release. Choose a
version newer than any published release with a stable version number, and tag
the protected `main` commit. The first release must be at least `v0.2.0`
because older CI builds installed as `0.1.<run number>` must be able to
upgrade.

```sh
git fetch origin main
git tag -a v0.2.1 -m "Gesher 0.2.1" origin/main   # or a specific commit on main
git push origin v0.2.1
```

The tag starts a read-only **ci** run in a separate concurrency group from
branch, pull request, and manual runs. It validates the pushed tag, builds the
installer as exactly `0.2.1`, and runs the full package, upgrade, E2E, and
aggregate checks. Branch and pull request runs, manual runs on any ref, and
standalone fork runs produce CI-versioned installers and cannot publish.

After successful tag CI, **Publish Gesher release** runs from its protected
default-branch workflow revision. It verifies the official repository, CI
definition, exact run and attempt, tag-push validation step, current tag,
`main` ancestry, successful `ci-required` gate, and package artifact receipt.
It checks the artifact's archive SHA-256 and then the one installer and checksum
inside it. The publisher handles those files as data.

The publisher serializes its work across tags. It creates a draft with generated
notes, verifies the GitHub asset digests for both files, and reads the release
catalog and exact Git tag immediately before making the draft public. GitHub
does not offer an atomic update conditioned on the observed Git ref or release
catalog. Keep the tag and release unchanged throughout publication, and wait
for the publisher before starting a manual release.

If publication is interrupted, rerun the publisher for the same successful CI
attempt after inspecting its result. It can resume a draft when the prior
protected publisher's job log authenticates its release ID and provenance,
and every existing asset matches the tested bytes. It uploads only missing
assets. It leaves manual, unknown, unprovable, and mismatched drafts untouched.
If the publish request's response was lost, the publisher reconciles against
the exact release and asset identities. Inspect that release if GitHub is
temporarily unable to confirm the outcome.

When the protected CI definition changes before the publisher starts, the
mismatched run is refused. Use a separate version tag on a commit with the
current protected CI definition, and preserve any existing draft for inspection.
Until a suitable release exists, a successful updater check reports that no
release is available.

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

The early `windows-host-contracts` CI matrix runs these checks in Windows
PowerShell 5.1 on Windows 2022 and 2025. The existing package checks also verify that the installed updater
and version metadata are present. The real installer upgrade test preserves app
data and update settings while an unrelated process holds the old Explorer shell
extension open. Existing full Windows/framework/E2E and source/artifact identity
gates remain enabled.

Portable PowerShell on Linux can exercise core behavior and parse the scripts;
it does not establish Windows PowerShell, WPF or Inno Setup behavior. Those require
the Windows jobs. A release that has not yet been published cannot be tested as
an actual public end-to-end download; offline fixtures cover its selection and
transfer contracts until such a release exists.
