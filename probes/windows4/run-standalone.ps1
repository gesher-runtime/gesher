param([string] $Directory = 'dist-windows4', [string] $Logs = 'windows4-probe-logs')
$ErrorActionPreference = 'Stop'
[void][IO.Directory]::CreateDirectory([IO.Path]::GetFullPath($Logs))
Write-Host "INFO Windows4 standalone native fixtures: source $env:GITHUB_SHA"
Write-Host 'INFO COM window fixture uses the actual production HWND/COM/CV queue; real base/eventfd and Android integration remain full-CI-only.'
$failures = 0
foreach ($probe in @('windows4_text_store_probe', 'windows4_ime_tsf_probe', 'windows4_input_pane_probe')) {
  $start = [Diagnostics.ProcessStartInfo]::new([IO.Path]::GetFullPath("$Directory/$probe.exe"))
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  $process = [Diagnostics.Process]::Start($start)
  $stdout = $process.StandardOutput.ReadToEndAsync()
  $stderr = $process.StandardError.ReadToEndAsync()
  $timedOut = -not $process.WaitForExit(45000)
  if ($timedOut) { $process.Kill($true); $process.WaitForExit() }
  $drained = $stdout.Wait(10000) -and $stderr.Wait(10000)
  $text = if ($drained) { $stdout.Result + $stderr.Result } else { 'Output pipe did not close' }
  [IO.File]::WriteAllText([IO.Path]::GetFullPath("$Logs/$probe.log"), $text)
  Write-Host $text
  if ($timedOut -or -not $drained -or $process.ExitCode -ne 0 -or $text -notmatch '(?m)^0 windows4 .* check\(s\) failed\s*$') {
    Write-Host "FAIL Windows4 standalone: $probe exit $($process.ExitCode), timeout $timedOut, drained $drained"
    $failures++
  } else { Write-Host "PASS Windows4 standalone: $probe" }
  $process.Dispose()
}
Write-Host "$failures Windows4 standalone fixture(s) failed"
exit $failures
