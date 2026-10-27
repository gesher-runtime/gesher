# The host mutates its CRT after the NDK library has bound its environ import.
# Guards compare the current CRT slot before any guest traversal, so the old
# snapshot is a safe failing control. All binaries are built/executed in CI.
Check 'exec4: live environ after host narrow/wide mutations, real ELF calls and posix_spawn' {
  $info = [Diagnostics.ProcessStartInfo]::new((Resolve-Path "$Dir/exec4_environment_test.exe").Path)
  $info.UseShellExecute = $false
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $info.ArgumentList.Add((NoDrive "$Dir/libexec4environment.so"))
  $info.ArgumentList.Add((NoDrive "$Dir/libexec2child.so"))
  $process = [Diagnostics.Process]::Start($info)
  $stdout = $process.StandardOutput.ReadToEndAsync()
  $stderr = $process.StandardError.ReadToEndAsync()
  $timedOut = -not $process.WaitForExit(180000)
  if ($timedOut) { $process.Kill($true); $process.WaitForExit() }
  $text = ''
  if ($stdout.Wait(5000)) { $text += $stdout.Result } else { $timedOut = $true; $text += '(stdout did not close)' }
  if ($stderr.Wait(5000)) { $text += $stderr.Result } else { $timedOut = $true; $text += '(stderr did not close)' }
  if ($timedOut) { $text += '(exec4 environment fixture timed out)' }
  $global:LASTEXITCODE = $(if ($timedOut) { -1 } else { $process.ExitCode })
  $process.Dispose()
  $text
} @('exec4: PASS stale CRT snapshot detected without dereferencing it',
    'exec4: PASS environ aliases, guest setenv/putenv/unsetenv and posix_spawn file actions',
    'exec4: PASS host narrow/wide changes reach relocated ELF imports and spawned children')
