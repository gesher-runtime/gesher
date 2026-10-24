# Compiled code and nterp (docs/compiled-code.md, ADR 0033). Dot-sourced by
# run-tests.ps1: NoDrive, $Dir, $root, $bcp and $failed are its.
#
# Each execution mode runs the stress test (tests/dex-codegen/Stress.java:
# implicit null checks, stack overflow, exceptions through compiled frames,
# GC with frames on the stack, threads and monitors, JNI, proxies) and the
# CPU-bound benchmark (Bench.java), whose checksum must be the same in all.
#   switch:  -Xint, the switch interpreter (GESHER_ART_INTERPRET_ONLY=1 too)
#   nterp:   no -Xint, no JIT: nterp for every method

$image = "-Ximage:$(NoDrive "$root/system")/framework/boot.art"
$codegenDex = NoDrive "$Dir/codegen-tests.dex"

function RunMode([string[]] $ModeArgs, [string[]] $Program, [hashtable] $Env = @{}) {
  $psi = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
  foreach ($a in @("-Xbootclasspath:$bcp", $image) + $ModeArgs + @('-cp', $codegenDex) + $Program) {
    $psi.ArgumentList.Add($a)
  }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.Environment['GESHER_LOG_EXECUTION'] = '1'
  foreach ($k in $Env.Keys) { $psi.Environment[$k] = $Env[$k] }
  $p = [Diagnostics.Process]::Start($psi)
  $stderr = $p.StandardError.ReadToEndAsync()
  $stdout = $p.StandardOutput.ReadToEnd()
  if (-not $p.WaitForExit(600000)) { $p.Kill(); $stdout += "`n(timed out)" }
  [pscustomobject]@{ Exit = $p.ExitCode; Out = $stdout + "`n" + $stderr.Result }
}

function ReportFailure([string] $Name, $r) {
  Write-Host ('FAIL  codegen: {0} (exit 0x{1:X8})' -f $Name, $r.Exit)
  $text = $r.Out
  if (Test-Path "$Dir/dalvikvm.map") {
    $text = $text | python "$PSScriptRoot/../../../tools/symbolize.py" "$Dir/dalvikvm.map" | Out-String
  }
  ($text -split "`n" | Select-Object -Last 80) | Out-Host
  $script:failed++
}

$modes = [ordered]@{
  'switch' = @{ Args = @('-Xint', '-Xusejit:false'); Env = @{}; Expect = 'gesher: execution: switch interpreter' }
  'interpret-only switch' = @{ Args = @('-Xusejit:false'); Env = @{ GESHER_ART_INTERPRET_ONLY = '1' };
                               Expect = 'gesher: execution: switch interpreter, implicit null checks off' }
  'nterp' = @{ Args = @('-Xusejit:false'); Env = @{}; Expect = 'gesher: execution: nterp, implicit null checks on, implicit stack overflow checks on' }
}

$bench = [ordered]@{}
foreach ($name in $modes.Keys) {
  $m = $modes[$name]
  $r = RunMode $m.Args @('gesher.Stress') $m.Env
  if ($r.Exit -eq 0 -and $r.Out.Contains('Stress all passed') -and $r.Out.Contains($m.Expect)) {
    Write-Host "PASS  codegen: stress test under $name ($($m.Expect))"
  } else {
    ReportFailure "stress test under $name (expected: $($m.Expect))" $r
  }
  if ($name -eq 'interpret-only switch') { continue }
  $r = RunMode $m.Args @('gesher.Bench', '3') $m.Env
  if ($r.Exit -eq 0 -and $r.Out -match 'Bench done checksum=(-?\d+)') {
    $sum = $Matches[1]
    $kernels = [ordered]@{}
    foreach ($line in $r.Out -split "`n") {
      if ($line -match '^bench (\S+) (\d+)') { $kernels[$Matches[1]] = [int]$Matches[2] }
    }
    $bench[$name] = @{ Sum = $sum; Kernels = $kernels }
  } else {
    ReportFailure "benchmark under $name" $r
  }
}

if ($bench.Count -ge 2) {
  $sums = @($bench.Values | ForEach-Object { $_.Sum } | Sort-Object -Unique)
  if ($sums.Count -eq 1) {
    Write-Host "PASS  codegen: benchmark checksum $($sums[0]) is the same under $($bench.Keys -join ', ')"
  } else {
    Write-Host "FAIL  codegen: benchmark checksums differ: $(($bench.Keys | ForEach-Object { "$_=$($bench[$_].Sum)" }) -join ' ')"
    $script:failed++
  }
  $kernelNames = @($bench[0].Kernels.Keys)
  Write-Host ("INFO  codegen: benchmark, best of 3 rounds, ms: " + (($bench.Keys | ForEach-Object { $_ }) -join ' / '))
  foreach ($k in $kernelNames) {
    Write-Host ("INFO  codegen:   {0,-14} {1}" -f $k, (($bench.Keys | ForEach-Object { $bench[$_].Kernels[$k] }) -join ' / '))
  }
}
