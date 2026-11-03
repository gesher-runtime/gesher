# Native processes from native code (docs/exec.md, ADR 0055): fork()+exec*(),
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
Check 'exec2: native processes from native code (fork+exec, system, popen, posix_spawn, signals, waitpid)' {
  & "$Dir/gesher-exec.exe" --elf $driver 'libexec2test.so' (NoDrive $child)
} @('exec2: PASS system runs the shell and returns its status',
    'exec2: PASS popen reads a command''s output',
    'exec2: PASS posix_spawn with a dup2 file action, output captured',
    'exec2: PASS fork+dup2+execv runs the child with a redirected pipe',
    'exec2: PASS waitpid decodes an exit code',
    'exec2: PASS kill() to another process runs its SIGUSR1 and SIGTERM handlers',
    'exec2: PASS signal 0 finds a running process',
    'exec2: PASS SIGKILL ends a process (WIFSIGNALED)',
    'exec2: SUMMARY pass=9 fail=0')

# exec*() outside a fork replaces the image: the launcher process exits with the
# child's status (17).
$null = & "$Dir/gesher-exec.exe" --elf $driver 'libexec2test.so' '--exec-replace' (NoDrive $child) 2>&1
if ($LASTEXITCODE -eq 17) {
  Write-Host 'PASS  exec2: execv() outside a fork replaces the image (exit 17)'
} else {
  Write-Host "FAIL  exec2: execv() replace: exit $LASTEXITCODE, not 17"
  $failed++
}

# fork()+exec as dart:io's Process.start does it (a Flutter app's UI thread): the
# fork child waits for its parent's byte before exec, exec closes the O_CLOEXEC
# fds. A regression hangs, so this runs with a time limit.
$dartLog = Join-Path $env:RUNNER_TEMP "exec2-dart-start-$(Get-Random).log"
$dart = Start-Process "$Dir/gesher-exec.exe" -ArgumentList @('--elf', "`"$driver`"", 'libexec2test.so', '--dart-start', "`"$(NoDrive $child)`"") `
  -NoNewWindow -PassThru -RedirectStandardOutput $dartLog -RedirectStandardError "$dartLog.err"
$dartExited = $dart.WaitForExit(60000)
if (-not $dartExited) { $dart.Kill($true); $dart.WaitForExit() }
$dartOut = (Get-Content -Raw $dartLog -ErrorAction SilentlyContinue) + (Get-Content -Raw "$dartLog.err" -ErrorAction SilentlyContinue)
$dartExpect = @('exec2: PASS a fork child waits for its parent''s byte before exec (dart:io Process.start)',
  'exec2: PASS exec closes the O_CLOEXEC fds: the exec-status pipe ends while the program runs',
  'exec2: PASS the parent''s byte for the child is not in the program''s output',
  'exec2: SUMMARY pass=3 fail=0')
$dartMissing = $dartExpect | Where-Object { -not "$dartOut".Contains($_) }
if ($dartExited -and $dart.ExitCode -eq 0 -and -not $dartMissing) {
  Write-Host 'PASS  exec2: fork+exec as dart:io''s Process.start (the child waits for its parent''s byte; exec closes O_CLOEXEC fds)'
} else {
  $why = if (-not $dartExited) { 'hung for 60 s' } else { "exit $($dart.ExitCode); missing: $($dartMissing -join ', ')" }
  Write-Host "FAIL  exec2: fork+exec as dart:io's Process.start ($why)"
  Write-Host $dartOut
  $failed++
}

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
