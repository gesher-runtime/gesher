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

# Every thread's stack of a process that hangs, with cdb if the runner has it.
function CodegenStacks([int] $Id) {
  $cdb = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\cdb.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1
  if (-not $cdb) { return '(no cdb.exe on this machine)' }
  $text = & $cdb.FullName -p $Id -c '.symopt+0x40; ~*k 40; qd' 2>&1 | Out-String
  $text -replace 'dalvikvm\+0x', 'dalvikvm.exe+0x'
}

# Runs dalvikvm; a run that takes more than `Seconds` is stopped, with its stacks.
function RunDalvikvm([string[]] $Arguments, [hashtable] $Env = @{}, [int] $Seconds = 240) {
  $psi = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
  foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.Environment['GESHER_LOG_EXECUTION'] = '1'
  foreach ($k in $Env.Keys) { $psi.Environment[$k] = $Env[$k] }
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $p = [Diagnostics.Process]::Start($psi)
  $stderr = $p.StandardError.ReadToEndAsync()
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $extra = ''
  if (-not $p.WaitForExit($Seconds * 1000)) {
    $extra = "`n(timed out after $Seconds s; its threads:)`n" + (CodegenStacks $p.Id)
    try { $p.Kill($true) } catch {}
    $null = $p.WaitForExit(30000)
  }
  $null = $stdout.Wait(30000)
  $null = $stderr.Wait(30000)
  $o = if ($stdout.IsCompleted) { $stdout.Result } else { '' }
  $e = if ($stderr.IsCompleted) { $stderr.Result } else { '' }
  [pscustomobject]@{ Exit = $(if ($p.HasExited) { $p.ExitCode } else { -1 }); Out = $o + "`n" + $e + $extra;
                     Stdout = $o; Err = $e + $extra; Ms = [int]$watch.Elapsed.TotalMilliseconds }
}

function RunMode([string[]] $ModeArgs, [string[]] $Program, [hashtable] $Env = @{}) {
  RunDalvikvm (@("-Xbootclasspath:$bcp", $image) + $ModeArgs + @('-cp', $codegenDex) + $Program) $Env
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

# First, Hello under nterp with every fault traced (fault_signals.cc): the
# runtime's own faults, before the heavier checks.
$r = RunDalvikvm @("-Xbootclasspath:$bcp", $image, '-Xusejit:false', '-cp', (NoDrive "$Dir/hello.dex"), 'gesher.Hello') `
  @{ GESHER_TRACE_SIGNALS = '1'; GESHER_ART_NO_AOT = '1' } 60
if ($r.Exit -eq 0 -and $r.Out.Contains('Hello from ART on Windows') -and $r.Out.Contains('gesher: execution: nterp')) {
  Write-Host "PASS  codegen: Hello under nterp ($(@($r.Out -split "`n" | Where-Object { $_ -match 'gesher-signal:' }).Count) faults traced)"
  $nterpOk = $true
} else {
  ReportFailure 'Hello under nterp' $r
  $nterpOk = $false
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

# Without a working nterp, the compiled modes would only time out one by one.
if (-not $nterpOk) {
  Write-Host 'INFO  codegen: nterp does not run Hello; only the interpreter modes run'
  foreach ($k in 'nterp', 'aot', 'jit') { $modes.Remove($k) }
}

# A boot image without compiled code (--compiler-filter=verify) has no AOT mode.
if ($nterpOk) {
  $probe = RunMode @('-Xusejit:false') @('gesher.Bench', '0') @{}
  if ($probe.Out -match 'gesher: boot image: 0 methods with compiled code') {
    Write-Host 'INFO  codegen: the boot image has no compiled code; no aot mode'
    $modes.Remove('aot')
  }
}

# The switch interpreter and interpret-only mode recover from a deep-recursion
# StackOverflowError (the "so" variant); nterp and the JIT do not yet (ADR 0033).
$soModes = @('switch', 'interpret-only')

$bench = [ordered]@{}
foreach ($name in @($modes.Keys)) {
  $m = $modes[$name]
  $stressArgs = if ($soModes -contains $name) { @('gesher.Stress', 'so') } else { @('gesher.Stress') }
  $r = RunMode $m.Args $stressArgs $m.Env
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
    $rr = RunDalvikvm (@("-Xbootclasspath:$bcp", $image) + $m.Args + $extra + @('-cp', (NoDrive $dex.FullName), 'Main')) $m.Env
    $actual = $rr.Stdout -replace "`r", ''
    $results[$name] = @{ Ok = ($rr.Exit -eq 0 -and $actual.TrimEnd() -eq $expected.TrimEnd()); Exit = $rr.Exit; Out = $actual; Err = $rr.Err }
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
