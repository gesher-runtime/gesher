Checks of one line of work each, run by `run-tests.ps1` in its own scope
(name order): `NN-<track>.ps1`, using `Check`, `RunDex`, `NoDrive`, `$Dir`,
`$root` and `$bcp` from there. Keeping them apart lets branches add checks
without touching each other's lines.

CI runs seven isolated shards on each of Windows 2022 and Windows 2025:

- `native`, `desktop`, `windowing`, `services`, `contracts`, `media`, and `compilation`.

`../test-shards.json` assigns every `.ps1` file in this directory to one owner.
A new, missing or multiply assigned track fails preflight rather than silently
losing coverage. Each shard preserves name order and runs the common VM/framework
smoke checks. Tracks 30 and 32 also run in each shard to prepare the graphics
helper and the actual system fonts. Shared process helpers are loaded from their
original function definitions without running their owner track's tests.

```powershell
pwsh -NoProfile -File tests/windows/run-tests.ps1 dist -Shard native
pwsh -NoProfile -File tests/windows/run-tests.ps1 dist -Shard native -ListTracks
pwsh -NoProfile -File tests/windows/test-test-shards.ps1
```

Without `-Shard`, the complete suite still runs in name order. The existing
`-FrameworkContractsOnly` local mode also remains available. The required
`ci-required` check waits for every Windows shard to succeed. Each shard uploads
its own results. `TRACK_START` and `TRACK_END` record durations for subsequent
balancing.

### Runtime file contracts

Track 50 requires `PositionedIoTest` through real libcore JNI. Track 94 also
requires fresh-VM temporary-directory operations and actual UNC file/temp I/O.
The UNC fixture creates one unique, hidden, temporary localhost SMB share over
an owned disposable directory, permits only the invoking Windows account, and
removes the share and files in `finally`. It restores `TMPDIR`, `TMP`, `TEMP` and
the Server service if it started that service. These checks require elevated
Windows test privileges and the SMB server cmdlets/service, as available on the
Windows 2022/2025 CI runners. Setup, operation and cleanup failures fail the
check; they are not treated as absent-feature passes. The services shard runs these same assertions with the graphics/font prerequisites.
