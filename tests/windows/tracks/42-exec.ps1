# Android ELF executables as processes (docs/exec.md, ADR 0035). Dot-sourced
# by run-tests.ps1, so Check, NoDrive, $Dir, $root, $bcp and $failed are in
# scope. The test programs come from .github/workflows/exec.yml (the "exec"
# artifact): libexectest.so, an NDK-built PIE executable with a DT_NEEDED
# libexechelper.so, and exec-test.dex (gesher.ExecTest).

if (-not (Test-Path "$Dir/libexectest.so")) {
  Write-Host 'FAIL  exec: the exec artifact (libexectest.so) is missing'
  $failed++
  return
}

# gesher-exec.exe on its own: the ELF executable as the main program
# (argv, environ, auxv, its DT_NEEDED library through LD_LIBRARY_PATH, init
# functions with argc/argv/envp, a thread, emulated TLS).
$env:LD_LIBRARY_PATH = NoDrive $Dir
$env:EXEC_TEST_VAR = 'from the launcher'
Check 'exec: gesher-exec runs an NDK-built ELF executable as its main program' {
  & "$Dir/gesher-exec.exe" --elf (Resolve-Path "$Dir/libexectest.so").Path 'libexectest.so' echo one
} @('argc=3', 'argv[0]=<libexectest.so>', 'argv[2]=<one>', 'getenv=<from the launcher>', 'environ has EXEC_TEST_VAR=1',
    'pagesz=4096 phdr=set random=set', 'ctor argc=3', 'helper=<hello from libexechelper (constructor ran)>',
    'thread=100 tls=7', 'echo done')
Remove-Item Env:LD_LIBRARY_PATH, Env:EXEC_TEST_VAR

# Exit statuses from the launcher itself: exit(n), abort(), a fault.
$statusProblems = @()
foreach ($case in @(@('exit', '3', 3), @('abort', '', 134), @('segv', '', 139))) {
  $a = @('--elf', (Resolve-Path "$Dir/libexectest.so").Path, 'libexectest.so', $case[0])
  if ($case[1]) { $a += $case[1] }
  $env:LD_LIBRARY_PATH = NoDrive $Dir
  $o = & "$Dir/gesher-exec.exe" @a 2>&1 | Out-String
  $code = $LASTEXITCODE
  Remove-Item Env:LD_LIBRARY_PATH
  if ($code -ne $case[2]) { $statusProblems += "$($case[0]): exit $code, not $($case[2]) ($($o.Trim()))" }
}
if (-not $statusProblems) {
  Write-Host 'PASS  exec: gesher-exec exit statuses: exit(3) is 3, abort() is 134, a fault is 139'
} else {
  Write-Host "FAIL  exec: gesher-exec exit statuses: $($statusProblems -join '; ')"
  $failed++
}

# The minimal sh.
$shOut = & "$Dir/gesher-exec.exe" --sh -c 'echo hi $EXEC_NONE there && false || exit 3' 2>&1 | Out-String
$shCode = $LASTEXITCODE
if ($shCode -eq 3 -and $shOut.Trim() -eq 'hi there') {
  Write-Host 'PASS  exec: gesher-exec --sh -c runs simple commands with &&, || and exit'
} else {
  Write-Host "FAIL  exec: gesher-exec --sh -c (exit $shCode): $shOut"
  $failed++
}

# ProcessBuilder and Runtime.exec in dalvikvm through libcore's UNIXProcess
# natives (compat/libcore/process.cc): the ELF executable in gesher-exec,
# pipes both ways, waitFor before reading (as Simon Tatham's Puzzles does),
# exit statuses, destroy(), redirects, errors, sh, cmd.exe and a .cmd file.
Check 'exec: ProcessBuilder runs an ELF executable (stdin/stdout/stderr pipes, environment, working dir, exit status, destroy), sh and Windows programs' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/exec-test.dex") gesher.ExecTest (NoDrive (Resolve-Path $Dir).Path)
} @('ExecTest all passed')
