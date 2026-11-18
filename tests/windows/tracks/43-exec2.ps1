# Native processes from native code (docs/exec.md, ADR 0055): exec*(),
# system(), popen(), posix_spawn(), signals between processes, and waitpid's
# status decoding, plus the /system/bin tools gesher-exec provides. Dot-sourced
# by run-tests.ps1 (Check, NoDrive, $Dir, $failed are in scope). The test
# programs come from .github/workflows/exec.yml (the "exec" artifact):
# libexec2test.so (the driver) and libexec2child.so (the program it starts).

if (-not (Test-Path "$Dir/libexec2test.so")) {
  Write-Host 'FAIL  exec2: the exec2 artifact (libexec2test.so) is missing'
  $failed++
  return
}

$child = (Resolve-Path "$Dir/libexec2child.so").Path
$driver = (Resolve-Path "$Dir/libexec2test.so").Path
$env:LD_LIBRARY_PATH = NoDrive $Dir

# The driver runs the whole suite and prints one PASS line per feature and a
# SUMMARY; it exits 0 only if every check passed.
Check 'exec2: native processes from native code (system, popen, posix_spawn, signals, waitpid)' {
  & "$Dir/gesher-exec.exe" --elf $driver 'libexec2test.so' (NoDrive $child)
} @('exec2: PASS system runs the shell and returns its status',
    'exec2: PASS popen reads a command''s output',
    'exec2: PASS posix_spawn with a dup2 file action, output captured',
    'exec2: PASS waitpid decodes an exit code',
    'exec2: PASS kill() to another process runs its SIGUSR1 and SIGTERM handlers',
    'exec2: PASS signal 0 finds a running process',
    'exec2: PASS SIGKILL ends a process (WIFSIGNALED)',
    'exec2: SUMMARY pass=8 fail=0')

# exec*() outside a fork replaces the image: the launcher process exits with the
# child's status (17).
$null = & "$Dir/gesher-exec.exe" --elf $driver 'libexec2test.so' '--exec-replace' (NoDrive $child) 2>&1
if ($LASTEXITCODE -eq 17) {
  Write-Host 'PASS  exec2: execv() outside a fork replaces the image (exit 17)'
} else {
  Write-Host "FAIL  exec2: execv() replace: exit $LASTEXITCODE, not 17"
  $failed++
}

# The runtime cannot clone a Windows address space with Linux fork/vfork
# semantics. Unsupported calls must fail honestly and leave real I/O alone.
Check 'exec2: fork and vfork report ENOSYS without synthetic children or input' {
  & "$Dir/gesher-exec.exe" --elf $driver 'libexec2test.so' '--fork-unsupported'
} @('exec2: PASS fork reports ENOSYS without creating a child',
    'exec2: PASS vfork reports ENOSYS without creating a child',
    'exec2: PASS unsupported fork leaves no waitable child',
    'exec2: PASS unsupported fork leaves the caller and real pipe bytes unchanged',
    'exec2: SUMMARY pass=4 fail=0')

# Real child/grandchild descriptor sharing and separate-open atomic append.
# The expected count includes every public contract assertion in the corpus.
Check 'exec2: OFDs survive real spawn and independent processes append atomically' {
  & "$Dir/gesher-exec.exe" --elf $driver 'libexec2test.so' '--ofd-process' (NoDrive $child)
} @('exec2: PASS spawn preserves flags, aliases, shared position and CLOEXEC',
    'exec2: PASS parent F_SETFL reaches the child and a real grandchild',
    'exec2: PASS atomic append loses or overwrites no complete record across processes',
    'exec2: SUMMARY pass=11 fail=0')

# The /system/bin programs gesher-exec provides (docs/exec.md): the minimal
# shell runs them as external commands.
$lsOut = & "$Dir/gesher-exec.exe" --sh -c "ls $(NoDrive $Dir)" 2>&1 | Out-String
if ($lsOut -match 'libexec2test\.so') {
  Write-Host 'PASS  exec2: /system/bin/ls lists a directory'
} else {
  Write-Host "FAIL  exec2: ls: $($lsOut.Trim())"
  $failed++
}

$idOut = & "$Dir/gesher-exec.exe" --sh -c 'id' 2>&1 | Out-String
if ($idOut -match 'uid=\d+') {
  Write-Host 'PASS  exec2: /system/bin/id prints the uid'
} else {
  Write-Host "FAIL  exec2: id: $($idOut.Trim())"
  $failed++
}

# getprop reads $ANDROID_ROOT/build.prop; with none set it falls back to the
# default argument, as a device's getprop does.
$propOut = & "$Dir/gesher-exec.exe" --sh -c 'getprop ro.none.here fallback-value' 2>&1 | Out-String
if ($propOut -match 'fallback-value') {
  Write-Host 'PASS  exec2: /system/bin/getprop returns a default for an unset property'
} else {
  Write-Host "FAIL  exec2: getprop: $($propOut.Trim())"
  $failed++
}

Remove-Item Env:LD_LIBRARY_PATH
