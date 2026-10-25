# Compiled code and nterp (docs/compiled-code.md, ADR 0033). Dot-sourced by
# run-tests.ps1: NoDrive, $Dir, $root, $bcp and $failed are its.
#
# Each execution mode runs the stress test (tests/dex-codegen/Stress.java:
# implicit null checks, stack overflow, exceptions through compiled frames,
# GC with frames on the stack, threads and monitors, JNI, proxies) and the
# CPU-bound benchmark (Bench.java), whose checksum must be the same in all:
#   switch:  -Xint, the switch interpreter
#   nterp:   no JIT: app code on nterp, boot library on AOT code
#   jit:     the JIT on for app code, boot library on AOT code
# and the stress test once more under GESHER_ART_INTERPRET_ONLY=1 (the
# diagnosis switch: everything as before compiled code). The boot image's
# primary image is AOT-compiled (--compiler-filter=speed), so the nterp and
# jit modes exercise AOT code (boot image compiled-method count > 0).

# gesher-base's mmap layer first (probes/posix/mman_probe.cc, ADR 0036): the
# oat files' code and the JIT's dual view are mapped through it.
$probe = & "$Dir/mman_probe.exe" 2>&1 | Out-String
if ($LASTEXITCODE -eq 0 -and $probe.Contains('mman probe: all passed')) {
  $n = @($probe -split "`n" | Where-Object { $_ -match '^ok ' }).Count
  Write-Host "PASS  codegen: mman probe ($n checks: placeholder mappings, MAP_FIXED over mappings, executable file views, shared dual views)"
} else {
  Write-Host "FAIL  codegen: mman probe (exit $LASTEXITCODE)"
  Write-Host $probe
  $script:failed++
}

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

# First, Hello under nterp with every fault traced (fault_signals.cc): the app
# runs on nterp and calls into the boot image's AOT code, before the heavier
# checks.
$r = RunDalvikvm @("-Xbootclasspath:$bcp", $image, '-Xusejit:false', '-cp', (NoDrive "$Dir/hello.dex"), 'gesher.Hello') `
  @{ GESHER_TRACE_SIGNALS = '1' } 60
$aot = $r.Out -match 'gesher: boot image: [1-9]\d* methods with compiled code'
if ($r.Exit -eq 0 -and $r.Out.Contains('Hello from ART on Windows') -and $r.Out.Contains('gesher: execution: nterp')) {
  Write-Host "PASS  codegen: Hello on nterp + AOT boot image ($(@($r.Out -split "`n" | Where-Object { $_ -match 'gesher-signal:' }).Count) faults traced, AOT: $aot)"
  $nterpOk = $true
} else {
  ReportFailure 'Hello on nterp + AOT boot image' $r
  $nterpOk = $false
}

# What a mode's output must show, besides the program's own result. nterp and
# jit run app code on nterp / the JIT and the boot library on AOT code.
function ModeOk([string] $Name, [string] $Out) {
  switch ($Name) {
    'switch' { return $Out.Contains('gesher: execution: switch interpreter') }
    'interpret-only' { return $Out.Contains('gesher: execution: switch interpreter, implicit null checks off, implicit stack overflow checks off') }
    'nterp' { return $Out.Contains('gesher: execution: nterp, implicit null checks on, implicit stack overflow checks on') }
    'jit' { return $Out -match 'Current number of JIT code cache entries: [1-9]' }
  }
  return $false
}

$jitArgs = @('-Xusejit:true', '-XX:DumpJITInfoOnShutdown')
$modes = [ordered]@{
  'switch' = @{ Args = @('-Xint', '-Xusejit:false'); Env = @{} }
  'interpret-only' = @{ Args = @('-Xusejit:false'); Env = @{ GESHER_ART_INTERPRET_ONLY = '1' }; NoBench = $true }
  'nterp' = @{ Args = @('-Xusejit:false'); Env = @{} }
  'jit' = @{ Args = $jitArgs; Env = @{} }
}

# Without a working nterp, the compiled modes would only time out one by one.
if (-not $nterpOk) {
  Write-Host 'INFO  codegen: nterp does not run Hello; only the interpreter modes run'
  foreach ($k in 'nterp', 'jit') { $modes.Remove($k) }
}

# Every mode recovers from a deep-recursion StackOverflowError (the stress
# test's "so" variant; ADR 0036).
$bench = [ordered]@{}
foreach ($name in @($modes.Keys)) {
  $m = $modes[$name]
  $r = RunMode $m.Args @('gesher.Stress', 'so') $m.Env
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

# Deep recursion (tests/dex-codegen/StackOverflow.java, ADR 0036): app code,
# the boot library's AOT code (ArrayList.hashCode of a list that contains
# itself) and app code with a native call in every frame each overflow the
# stack four times in a row, on the main thread, on new threads (one with a
# 512 KiB stack) and on two threads at once; every StackOverflowError is
# caught and the thread goes on. Under jit, -verbose:jit shows the recursion
# compiled (the later rounds run the JIT's code).
foreach ($name in @($modes.Keys)) {
  $m = $modes[$name]
  $soArgs = $m.Args
  if ($name -eq 'jit') { $soArgs = $soArgs + @('-verbose:jit') }
  $r = RunMode $soArgs @('gesher.StackOverflow') $m.Env
  $jitted = $name -ne 'jit' -or $r.Out -match 'Compiling method .*Recurser\.recurse'
  if ($r.Exit -eq 0 -and $r.Out.Contains('StackOverflow all passed') -and (ModeOk $name $r.Out) -and $jitted) {
    $depths = ($r.Out -split "`n" | Where-Object { $_ -match '^ok   stack overflow, (\w+) code, on the main thread \(depths ([\d ]+)\)' } |
      ForEach-Object { "$($Matches[1]) $($Matches[2])" }) -join '; '
    if ($name -eq 'jit') {
      # The JIT's code cache: one file, a writable view and an executable one.
      $region = ($r.Out -split "`n" | Where-Object { $_ -match 'Created JitMemoryRegion' } | Select-Object -First 1)
      if ($region -match 'non_exec_pages=0x[0-9a-f]*[1-9a-f]' -and $r.Out -notmatch 'Failed to initialize dual view JIT') {
        Write-Host "PASS  codegen: the JIT code cache is dual-mapped, W^X ($(($region -replace '^.*Created JitMemoryRegion: ', '').Trim()))"
      } else {
        Write-Host "FAIL  codegen: the JIT code cache is not dual-mapped ($region)"
        $script:failed++
      }
    }
    Write-Host "PASS  codegen: StackOverflowError caught and recovered from under $name (main thread, new threads, 2 at once; app, boot, native code; 4 rounds each; main thread depths: $depths)"
  } else {
    ReportFailure "StackOverflowError under $name$(if (-not $jitted) { ' (the recursion was not JIT-compiled)' })" $r
  }
}

# Without placeholder mappings (GESHER_MMAP_NO_PLACEHOLDERS=1, as on Windows
# before 1803) gesher-base's memfd_create fails for ART (patch 0017), and the
# JIT takes its single mapping: the stress test still passes under the JIT.
if ($modes.Contains('jit')) {
  $r = RunMode ($jitArgs + @('-verbose:jit')) @('gesher.Stress', 'so') @{ GESHER_MMAP_NO_PLACEHOLDERS = '1' }
  $region = ($r.Out -split "`n" | Where-Object { $_ -match 'Created JitMemoryRegion' } | Select-Object -First 1)
  if ($r.Exit -eq 0 -and $r.Out.Contains('Stress all passed') -and (ModeOk 'jit' $r.Out) -and $region -notmatch 'non_exec_pages=0x[0-9a-f]*[1-9a-f]') {
    Write-Host "PASS  codegen: stress test under jit without placeholder mappings (single-mapped JIT code cache)"
  } else {
    ReportFailure 'stress test under jit without placeholder mappings' $r
  }
}

# JNI stubs (tests/dex-codegen/JniStubs.java, ADR 0036): jni_test.so's natives
# (tests/native) called 40,000 times a round from two threads. Natives whose
# shorty a boot image JNI stub has use that AOT stub; under jit the JIT
# compiles the others' (-verbose:jit: "JIT added ... gesher.JniTest.mix", 15
# arguments, some on the stack). The boot library's natives have the boot
# image's AOT JNI stubs (the count is in the "boot image:" line).
$jniLib = Join-Path $Dir 'jni_test.so'
if (Test-Path $jniLib) {
  foreach ($name in @($modes.Keys)) {
    if ($name -eq 'interpret-only') { continue }
    $m = $modes[$name]
    $jniArgs = $m.Args
    if ($name -eq 'jit') { $jniArgs = $jniArgs + @('-verbose:jit') }
    $r = RunMode $jniArgs @('gesher.JniStubs', (NoDrive (Resolve-Path $jniLib).Path)) $m.Env
    $stubs = @($r.Out -split "`n" | Where-Object { $_ -match 'JIT added .*gesher\.JniTest\.' } |
      ForEach-Object { if ($_ -match '\) (\S+ gesher\.JniTest\.\w+)') { $Matches[1] } } | Sort-Object -Unique)
    $jitOk = $name -ne 'jit' -or $stubs.Count -gt 0
    $bootJni = if ($r.Out -match 'boot image: \d+ methods with compiled code \((\d+) native') { $Matches[1] } else { '?' }
    if ($r.Exit -eq 0 -and $r.Out.Contains('JniStubs all passed') -and (ModeOk $name $r.Out) -and $jitOk) {
      $what = if ($name -eq 'jit') { "JIT-compiled JNI stubs: $($stubs -join ', ')" } else { 'generic JNI for app natives' }
      Write-Host "PASS  codegen: JNI calls under $name ($what; boot image JNI stubs: $bootJni)"
    } else {
      ReportFailure "JNI calls under $name$(if (-not $jitOk) { ' (no JIT-compiled JNI stub for a JniTest native)' })" $r
    }
  }
} else {
  Write-Host "FAIL  codegen: no jni_test.so in $Dir (the native-libs artifact)"
  $script:failed++
}

# ART's own run-tests (tests/build-art-run-tests.sh): each runs under the
# switch interpreter first; one that passes there must pass under every
# other mode too. (One that fails there fails for reasons of its own, not
# compiled code's; it is reported, not counted.)
$runTestArgs = @{ '080-oom-throw' = @('-Xmx16m') }
foreach ($dex in Get-ChildItem "$Dir/art-*.dex" -ErrorAction SilentlyContinue | Sort-Object Name) {
  $test = $dex.BaseName.Substring(4)
  $expected = (([string](Get-Content -Raw "$Dir/art-$test.expected-stdout.txt")) -replace "`r", '').TrimEnd()
  $extra = if ($runTestArgs.ContainsKey($test)) { $runTestArgs[$test] } else { @() }
  $results = [ordered]@{}
  foreach ($name in @($modes.Keys)) {
    if ($name -eq 'interpret-only') { continue }
    $m = $modes[$name]
    $rr = RunDalvikvm (@("-Xbootclasspath:$bcp", $image) + $m.Args + $extra + @('-cp', (NoDrive $dex.FullName), 'Main')) $m.Env
    $actual = ([string]$rr.Stdout) -replace "`r", ''
    $results[$name] = @{ Ok = ($rr.Exit -eq 0 -and $actual.TrimEnd() -eq $expected); Exit = $rr.Exit; Out = $actual; Err = $rr.Err }
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
