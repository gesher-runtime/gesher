# Binder across processes and the framework's system services
# (docs/services.md, docs/decisions/0014-services-binder-ipc.md). Dot-sourced
# by run-tests.ps1, whose Check, NoDrive, $Dir, $root, $bcp and $failed these
# use.

# Runs a program with a time limit (binder calls that go wrong tend to wait
# forever), killing it and whatever it started when the time is up. Returns
# its output; $LASTEXITCODE is its exit code (-1 on a time-out).
function RunTimed([string] $Exe, [string[]] $Arguments = @(), [int] $Seconds = 120) {
  $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
  foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = [Diagnostics.Process]::Start($psi)
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEndAsync()
  $timedOut = -not $p.WaitForExit($Seconds * 1000)
  if ($timedOut) {
    $p.Kill($true)
    $p.WaitForExit()
  }
  $null = $stdout.Wait(10000), $stderr.Wait(10000)
  $global:LASTEXITCODE = if ($timedOut) { -1 } else { $p.ExitCode }
  $out = $stdout.Result + $stderr.Result
  if ($timedOut) { $out += "`n(timed out after $Seconds s)" }
  return $out
}

function CheckTimed([string] $Name, [string] $Exe, [string[]] $Arguments, [string[]] $Expect, [int] $Seconds = 120) {
  $out = RunTimed $Exe $Arguments $Seconds
  $code = $LASTEXITCODE
  $missing = $Expect | Where-Object { -not $out.Contains($_) }
  if ($code -eq 0 -and -not $missing) {
    Write-Host "PASS  $Name"
  } else {
    Write-Host "FAIL  $Name (exit $code; missing: $($missing -join ', '))"
    Write-Host $out
    $script:failed++
  }
}

# The driver's ABI across processes: binderd, a context manager and a client
# with nested calls, objects passed both ways, and a death notification.
CheckTimed 'binder: /dev/binder across processes (binderd, raw ioctls)' "$Dir/binder_probe.exe" @() @(
  'binder probe: all passed')

# libbinder over it, with frameworks/native's servicemanager.
CheckTimed 'binder: servicemanager and libbinder across processes' "$Dir/libbinder_probe.exe" @() @(
  'libbinder probe: all passed')
