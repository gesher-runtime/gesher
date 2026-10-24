# Compiled code and nterp (docs/compiled-code.md, ADR 0033). Dot-sourced by
# run-tests.ps1: NoDrive, $Dir, $root, $bcp and $failed are its.
#
# Each execution mode runs the stress test (tests/dex-codegen/Stress.java:
# implicit null checks, stack overflow, exceptions through compiled frames,
# GC with frames on the stack, threads and monitors, JNI, proxies) and the
# CPU-bound benchmark (Bench.java), whose checksum must be the same in all:
#   switch:  -Xint, the switch interpreter
#   nterp:   GESHER_ART_NO_AOT=1, no JIT: nterp for every method
#   aot:     no JIT: the boot image's compiled code, nterp for the rest
#   jit:     the JIT on, over the boot image's compiled code
# and the stress test once more under GESHER_ART_INTERPRET_ONLY=1 (the
# diagnosis switch: everything as before compiled code).

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
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $p = [Diagnostics.Process]::Start($psi)
  $stderr = $p.StandardError.ReadToEndAsync()
  $stdout = $p.StandardOutput.ReadToEnd()
  if (-not $p.WaitForExit(600000)) { $p.Kill(); $stdout += "`n(timed out)" }
  [pscustomobject]@{ Exit = $p.ExitCode; Out = $stdout + "`n" + $stderr.Result; Ms = [int]$watch.Elapsed.TotalMilliseconds }
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

# What a mode's output must show, besides the program's own result.
function ModeOk([string] $Name, [string] $Out) {
  switch ($Name) {
    'switch' { return $Out.Contains('gesher: execution: switch interpreter') }
    'interpret-only' { return $Out.Contains('gesher: execution: switch interpreter, implicit null checks off, implicit stack overflow checks off') }
    'nterp' { return $Out.Contains('gesher: execution: nterp, implicit null checks on, implicit stack overflow checks on') }
    'aot' { return $Out.Contains('gesher: execution: nterp') -and $Out -match 'gesher: boot image: [1-9]\d* methods with compiled code' }
    'jit' { return $Out -match 'Current number of JIT code cache entries: [1-9]' }
  }
  return $false
}

$jitArgs = @('-Xusejit:true', '-XX:DumpJITInfoOnShutdown')
$modes = [ordered]@{
  'switch' = @{ Args = @('-Xint', '-Xusejit:false'); Env = @{} }
  'interpret-only' = @{ Args = @('-Xusejit:false'); Env = @{ GESHER_ART_INTERPRET_ONLY = '1' }; NoBench = $true }
  'nterp' = @{ Args = @('-Xusejit:false'); Env = @{ GESHER_ART_NO_AOT = '1' } }
  'aot' = @{ Args = @('-Xusejit:false'); Env = @{} }
  'jit' = @{ Args = $jitArgs; Env = @{} }
}

# A boot image without compiled code (--compiler-filter=verify) has no AOT mode.
$probe = RunMode @('-Xusejit:false') @('gesher.Bench', '0') @{}
if ($probe.Out -match 'gesher: boot image: 0 methods with compiled code') {
  Write-Host 'INFO  codegen: the boot image has no compiled code; no aot mode'
  $modes.Remove('aot')
}

$bench = [ordered]@{}
foreach ($name in @($modes.Keys)) {
  $m = $modes[$name]
  $r = RunMode $m.Args @('gesher.Stress') $m.Env
  if ($r.Exit -eq 0 -and $r.Out.Contains('Stress all passed') -and (ModeOk $name $r.Out)) {
    $what = ($r.Out -split "`n" | Where-Object { $_ -match 'gesher: (execution|boot image):|JIT code cache entries' } |
      ForEach-Object { ($_ -replace '^.*gesher: ', '').Trim() }) -join '; '
    Write-Host "PASS  codegen: stress test under $name ($what)"
  } else {
    ReportFailure "stress test under $name" $r
  }
  if ($m.NoBench) { continue }
  $r = RunMode $m.Args @('gesher.Bench', '3') $m.Env
  if ($r.Exit -eq 0 -and $r.Out -match 'Bench done checksum=(-?\d+)' -and (ModeOk $name $r.Out)) {
    $sum = $Matches[1]
    $kernels = [ordered]@{}
    foreach ($line in $r.Out -split "`n") {
      if ($line -match '^bench (\S+) (\d+)') { $kernels[$Matches[1]] = [int]$Matches[2] }
    }
    $bench[$name] = @{ Sum = $sum; Kernels = $kernels; Ms = $r.Ms }
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
  $first = @($bench.Values)[0]
  Write-Host ("INFO  codegen: benchmark (best of 3 rounds, ms): " + ($bench.Keys -join ' / '))
  foreach ($k in @($first.Kernels.Keys)) {
    Write-Host ("INFO  codegen:   {0,-14} {1}" -f $k, (($bench.Keys | ForEach-Object { $bench[$_].Kernels[$k] }) -join ' / '))
  }
  Write-Host ("INFO  codegen:   {0,-14} {1}" -f 'process', (($bench.Keys | ForEach-Object { $bench[$_].Ms }) -join ' / '))
}

# ART's own run-tests (tests/build-art-run-tests.sh): each runs under the
# switch interpreter first; one that passes there must pass under every
# other mode too. (One that fails there fails for reasons of its own, not
# compiled code's; it is reported, not counted.)
$runTestArgs = @{ '080-oom-throw' = @('-Xmx16m') }
foreach ($dex in Get-ChildItem "$Dir/art-*.dex" -ErrorAction SilentlyContinue | Sort-Object Name) {
  $test = $dex.BaseName.Substring(4)
  $expected = (Get-Content -Raw "$Dir/art-$test.expected-stdout.txt") -replace "`r", ''
  $extra = if ($runTestArgs.ContainsKey($test)) { $runTestArgs[$test] } else { @() }
  $results = [ordered]@{}
  foreach ($name in @($modes.Keys)) {
    if ($name -eq 'interpret-only') { continue }
    $m = $modes[$name]
    $psi = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
    foreach ($a in @("-Xbootclasspath:$bcp", $image) + $m.Args + $extra + @('-cp', (NoDrive $dex.FullName), 'Main')) {
      $psi.ArgumentList.Add($a)
    }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    foreach ($k in $m.Env.Keys) { $psi.Environment[$k] = $m.Env[$k] }
    $p = [Diagnostics.Process]::Start($psi)
    $err = $p.StandardError.ReadToEndAsync()
    $stdout = $p.StandardOutput.ReadToEnd()
    if (-not $p.WaitForExit(600000)) { $p.Kill(); $stdout += "`n(timed out)" }
    $actual = $stdout -replace "`r", ''
    $results[$name] = @{ Ok = ($p.ExitCode -eq 0 -and $actual.TrimEnd() -eq $expected.TrimEnd()); Exit = $p.ExitCode; Out = $actual; Err = $err.Result }
  }
  if (-not $results['switch'].Ok) {
    Write-Host ('INFO  codegen: art run-test {0} does not pass under the switch interpreter either (exit 0x{1:X8}); not counted' -f $test, $results['switch'].Exit)
    continue
  }
  $bad = @($results.Keys | Where-Object { -not $results[$_].Ok })
  if (-not $bad) {
    Write-Host "PASS  codegen: art run-test $test under $($results.Keys -join ', ')"
  } else {
    foreach ($b in $bad) {
      ReportFailure "art run-test $test under $b" ([pscustomobject]@{ Exit = $results[$b].Exit; Out = "--- stdout`n$($results[$b].Out)`n--- stderr`n$($results[$b].Err)" })
    }
  }
}
