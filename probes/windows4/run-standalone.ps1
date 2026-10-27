param([string] $Directory = 'dist-windows4', [string] $Logs = 'windows4-probe-logs')
$ErrorActionPreference = 'Stop'
Add-Type -Path "$PSScriptRoot/../../tests/windows/ime/ImeInput.cs"
Write-Host 'PASS Windows4 host helper: ImeInput C# compiles; real layout recovery remains a full-CI check'
[void][IO.Directory]::CreateDirectory([IO.Path]::GetFullPath($Logs))
Write-Host "INFO Windows4 standalone native fixtures: source $env:GITHUB_SHA"
Write-Host 'INFO COM window fixture uses the actual production HWND/COM/CV queue; real base/eventfd and Android integration remain full-CI-only.'
function Read-Probe([string] $Name) {
  $start = [Diagnostics.ProcessStartInfo]::new([IO.Path]::GetFullPath("$Directory/$Name.exe"))
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
  [IO.File]::WriteAllText([IO.Path]::GetFullPath("$Logs/$Name.log"), $text)
  Write-Host $text
  $result = [pscustomobject]@{ Text = $text; ExitCode = $process.ExitCode; TimedOut = $timedOut; Drained = $drained }
  $process.Dispose()
  return $result
}
$failures = 0
foreach ($probe in @('windows4_text_store_probe', 'windows4_ime_tsf_probe', 'windows4_input_pane_probe', 'windows4_initial_size_probe')) {
  $result = Read-Probe $probe
  if ($result.TimedOut -or -not $result.Drained -or $result.ExitCode -ne 0 -or $result.Text -match '(?m)^FAIL' -or
      $result.Text -notmatch '(?m)^0 windows4 .* check\(s\) failed\s*$') {
    Write-Host "FAIL Windows4 standalone: $probe exit $($result.ExitCode), timeout $($result.TimedOut), drained $($result.Drained)"
    $failures++
  } else { Write-Host "PASS Windows4 standalone: $probe" }
}
Write-Host 'INFO NEGATIVE COUNTERFACTUALS: deliberately changed source copies must fail exactly their declared regression assertions. Raw negative FAIL lines remain visible.'
$manifest = Get-Content -Raw -LiteralPath "$Directory/counterfactuals.json" | ConvertFrom-Json
if ($manifest.source_sha -cne $env:GITHUB_SHA -or @($manifest.variants).Count -ne 6 -or
    @($manifest.variants.name | Sort-Object -Unique).Count -ne 6) { throw 'Unexpected counterfactual provenance' }
foreach ($case in $manifest.variants) {
  if ($case.name -notin @('lease','caret','geometry','composition','initial-size','activation')) { throw 'Unexpected counterfactual name' }
  Write-Host "INFO NEGATIVE COUNTERFACTUAL $($case.name): $($case.source)"
  $result = Read-Probe "counterfactual-$($case.name)"
  $observed = @([regex]::Matches($result.Text, '(?m)^FAIL\s+windows4 [^:\r\n]+: ([^\r\n]+)') | ForEach-Object { $_.Groups[1].Value })
  $expected = @($case.expected_failures)
  $exact = $observed.Count -eq $expected.Count -and $result.ExitCode -eq $expected.Count
  foreach ($label in $expected) { if (@($observed | Where-Object { $_ -ceq $label }).Count -ne 1) { $exact = $false } }
  if ($result.TimedOut -or -not $result.Drained -or -not $exact) {
    Write-Host "FAIL Windows4 counterfactual: $($case.name) did not produce exactly its expected regression failures"
    $failures++
  } else { Write-Host "PASS Windows4 counterfactual: $($case.name) detects exactly $($expected.Count) expected regression failure(s)" }
}
Write-Host "$failures Windows4 standalone fixture(s) failed"
exit $failures
