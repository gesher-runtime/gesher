# Pinned bionic action/exec errors are real child127, after spawn0/PID.
foreach ($execSpawnAbi in 'x86_64', 'arm64-v8a') {
  $execSpawnProgram = NoDrive "$Dir/exec4-spawn-$execSpawnAbi"
  $execSpawnWork = Join-Path $root ("exec4-spawn-$execSpawnAbi-" + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $execSpawnWork -ErrorAction Stop | Out-Null
  CheckTimed "exec4 spawn ($execSpawnAbi): actual ordered actions, child127 and parent isolation" "$Dir/gesher-exec.exe" @(
    '--elf', $execSpawnProgram, 'exec4-spawn', $execSpawnProgram, (NoDrive $execSpawnWork)) @(
    "exec4 spawn ABI: $execSpawnAbi",
    'ordered relative open/dup/close/chdir/fchdir runs in the real child',
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
    '0 test(s) failed')
}
