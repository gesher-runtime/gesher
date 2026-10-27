# Real NDK action-registration calls, not a reconstruction of the shim list.
foreach ($execActionAbi in 'x86_64', 'arm64-v8a') {
  $execActionProgram = NoDrive "$Dir/exec4-actions-$execActionAbi"
  CheckTimed "exec4 actions ($execActionAbi): pinned registration errors and real child waits" "$Dir/gesher-exec.exe" @(
    '--elf', $execActionProgram, 'exec4-actions', $execActionProgram) @(
    "exec4 actions ABI: $execActionAbi",
    '15 negative descriptor registrations return EBADF without changing errno',
    'spawn after rejected actions runs a real child and waits for exit42',
    'spawnp after rejected actions runs a real child and waits for exit42',
    'positive unopened descriptors are accepted without testing parent fd availability',
    '0 test(s) failed')
}
