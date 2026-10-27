# The host mutates its CRT after the NDK library has bound its environ import.
# Guards compare the current CRT slot before any guest traversal, so the old
# snapshot is a safe failing control. All binaries are built/executed in CI.
Check 'exec4: live environ after host narrow/wide mutations, real ELF calls and posix_spawn' {
  & "$Dir/exec4_environment_test.exe" (NoDrive "$Dir/libexec4environment.so") (NoDrive "$Dir/libexec2child.so")
} @('exec4: PASS stale CRT snapshot detected without dereferencing it',
    'exec4: PASS environ aliases, guest setenv/putenv/unsetenv and posix_spawn file actions',
    'exec4: PASS host narrow/wide changes reach relocated ELF imports and spawned children')
