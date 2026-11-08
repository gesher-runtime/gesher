Checks of one line of work each, run by `run-tests.ps1` in its own scope
(name order): `NN-<track>.ps1`, using `Check`, `RunDex`, `NoDrive`, `$Dir`,
`$root` and `$bcp` from there. Keeping them apart lets branches add checks
without touching each other's lines.

For earlier framework and service feedback from the same build output, run:

```powershell
pwsh -NoProfile -File tests/windows/run-tests.ps1 dist -FrameworkContractsOnly
```

This keeps the runner's initial VM, native/framework and application lifecycle
checks, then runs the existing `30-graphics.ps1`, `32-fonts.ps1`,
`50-services.ps1` and `94-framework-fixes.ps1` in that order. The graphics track
provides the font-test runner; the font track installs and checks the actual
system fonts used by the service applications. Each selected track runs all its
existing checks, with the same failure counters, process exit checks and expected
markers. A missing required track fails the invocation. This mode does not
replace the full suite: omitting the switch still discovers and runs every track
in name order.

### Runtime file contracts

Track 50 requires `PositionedIoTest` through real libcore JNI. Track 94 also
requires fresh-VM temporary-directory operations and actual UNC file/temp I/O.
The UNC fixture creates one unique, hidden, temporary localhost SMB share over
an owned disposable directory, permits only the invoking Windows account, and
removes the share and files in `finally`. It restores `TMPDIR`, `TMP`, `TEMP` and
the Server service if it started that service. These checks require elevated
Windows test privileges and the SMB server cmdlets/service, as available on the
Windows 2022/2025 CI runners. Setup, operation and cleanup failures fail the
check; they are not treated as absent-feature passes. The framework-contracts
leg runs these same assertions before the complete suite finishes.
