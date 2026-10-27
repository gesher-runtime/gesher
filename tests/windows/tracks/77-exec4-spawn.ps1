# Pinned bionic action/exec errors are real child127, after spawn0/PID.
$execSpawnOldTrace = $env:GESHER_SPAWN_TRACE
$execSpawnOriginalDir = $Dir
$execSpawnPushedLocation = $false
try {
$execSpawnAbsoluteDir = (Resolve-Path -LiteralPath $Dir -ErrorAction Stop).Path
# Both canonical full and the scoped driver execute the same dual entry.
# Pin the parent cwd so the relative basename is genuinely relative, on the
# same drive as its absolute counterpart, with no silent absolute fallback.
Push-Location -LiteralPath (Split-Path -Parent $execSpawnAbsoluteDir)
$execSpawnPushedLocation = $true
foreach ($execSpawnDirCase in @(
  @{ Name='absolute'; Path=$execSpawnAbsoluteDir },
  @{ Name='relative'; Path=(Split-Path -Leaf $execSpawnAbsoluteDir) })) {
$Dir = $execSpawnDirCase.Path
Write-Host "EXEC4_DRIVER entry=$($execSpawnDirCase.Name) Dir=[$Dir] rooted=$([IO.Path]::IsPathRooted($Dir))"
$env:GESHER_SPAWN_TRACE = '1'
foreach ($execSpawnAbi in 'x86_64', 'arm64-v8a') {
  # Child actions intentionally change cwd before POSIX program lookup.
  $execSpawnProgram = NoDrive (Resolve-Path -LiteralPath "$Dir/exec4-spawn-$execSpawnAbi" -ErrorAction Stop).Path
  $execSpawnWork = Join-Path $root ("exec4-spawn-$execSpawnAbi-" + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $execSpawnWork -ErrorAction Stop | Out-Null
  CheckTimed "exec4 spawn [$($execSpawnDirCase.Name)] ($execSpawnAbi): actual ordered actions, child127 and parent isolation" "$Dir/gesher-exec.exe" @(
    '--elf', $execSpawnProgram, 'exec4-spawn', $execSpawnProgram, (NoDrive $execSpawnWork)) @(
    "exec4 spawn ABI: $execSpawnAbi",
    'ordered relative open/dup/close/chdir/fchdir runs in the real child',
    'INFO exec4 ordered child: argv1 fd301 fd321 parent1 copy1 relative1 pid1',
    'returned PID is the ELF program PID written by that child',
    'failed open returns spawn0 and a real child exit127',
    'missing dup2 source returns spawn0 and a real child exit127',
    'missing dup2 same-fd source returns spawn0 and a real child exit127',
    'failed chdir returns spawn0 and a real child exit127',
    'missing fchdir source returns spawn0 and a real child exit127',
    'regular-file fchdir returns spawn0 and a real child exit127',
    'first failed action prevents later file creation',
    'unopened positive close is ignored',
    'unmentioned inherited close-on-exec fd is closed',
    'same-fd dup clears child close-on-exec',
    'dup preserves an explicit copy and closes the flagged source',
    'fchdir uses an inherited flagged directory before exec closes it',
    'ABI-specific O_DIRECTORY rejects a regular file in addopen',
    'failed exec returns spawn0 and real child127 after file actions',
    'successful file action precedes failed program lookup',
    'spawnp searches caller PATH while passing exact argv and replacement envp',
    'every child preserves the parent directory and descriptor flags',
    'unmentioned closed parent stdin stays EBADF in the child',
    'dup2 from closed parent stdin fails in a real child with127',
    'closed-stdin failure prevents later file creation',
    '0 test(s) failed')
  CheckTimed "exec4 spawn [$($execSpawnDirCase.Name)] ($execSpawnAbi): actual UCRT no-console stdin remains closed" "$Dir/exec4_no_console_probe.exe" @(
    '--run', $execSpawnProgram) @(
    'PASS  spawn no-console: raw stdin -2, spawn0, actual child',
    'status0x2a00, parent unchanged')
}
}
} finally {
  $Dir = $execSpawnOriginalDir
  $env:GESHER_SPAWN_TRACE = $execSpawnOldTrace
  if ($execSpawnPushedLocation) { Pop-Location }
}
