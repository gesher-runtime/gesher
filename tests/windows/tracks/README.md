Checks of one line of work each, run by `run-tests.ps1` in its own scope
(name order): `NN-<track>.ps1`, using `Check`, `RunDex`, `NoDrive`, `$Dir`,
`$root` and `$bcp` from there. Keeping them apart lets branches add checks
without touching each other's lines.
