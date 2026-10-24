# Boot image (docs/boot-image.md, ADR 0031). run-tests.ps1 put the image from
# the boot-image job in $root/system/framework/x86_64, where every -Ximage of
# the checks before this one looks, and set GESHER_REQUIRE_BOOT_IMAGE, so each
# of those runs failed rather than fall back to imageless running. Here: what
# the runtime loads for each boot class path, and how long startup takes
# without the image and with it.

$imageDir = Join-Path $root 'system/framework/x86_64'
if (-not (Test-Path (Join-Path $imageDir 'boot.art'))) {
  Write-Host "FAIL  boot image: no boot.art in $imageDir (the boot-image artifact)"
  $script:failed++
  return
}
$image = "-Ximage:$(NoDrive "$root/system")/framework/boot.art"
# A location with no image: the runtime falls back to imageless running.
$noImage = "-Ximage:$(NoDrive "$root/system")/no-boot-image/boot.art"
$bcpFrameworkOnly = (('core-oj', 'core-libart', 'core-icu4j', 'framework') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$bcpWithApp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$helloArgs = @("-Xbootclasspath:$bcp", '-Xint', '-Xusejit:false', '-cp', (NoDrive "$Dir/hello.dex"), 'gesher.Hello')
$lifecycleArgs = @("-Xbootclasspath:$bcpWithApp", '-Xint', '-Xusejit:false', 'com.gesher.app.Main', (NoDrive "$Dir/lifecycle.apk"))

# Runs dalvikvm with `Image` and `Arguments`; returns when `Marker` appeared on
# stdout (ms from the process start), the run's total time, its exit code and output.
function Startup([string] $Image, [string[]] $Arguments, [string] $Marker, [bool] $Require) {
  $psi = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
  foreach ($a in @($Image) + $Arguments) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  if ($Require) { $psi.Environment['GESHER_REQUIRE_BOOT_IMAGE'] = '1' } else { $null = $psi.Environment.Remove('GESHER_REQUIRE_BOOT_IMAGE') }
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $p = [Diagnostics.Process]::Start($psi)
  $stderr = $p.StandardError.ReadToEndAsync()
  $at = $null
  $lines = [Collections.Generic.List[string]]::new()
  while ($null -ne ($line = $p.StandardOutput.ReadLine())) {
    if ($null -eq $at -and $line.Contains($Marker)) { $at = $watch.Elapsed.TotalMilliseconds }
    $lines.Add($line)
  }
  $p.WaitForExit()
  $total = $watch.Elapsed.TotalMilliseconds
  [pscustomobject]@{ At = $at; Total = $total; Exit = $p.ExitCode; Out = (($lines -join "`n") + "`n" + $stderr.Result) }
}

function Median([double[]] $xs) { $s = @($xs | Sort-Object); $s[[int][math]::Floor($s.Count / 2)] }

# What each boot class path loads: the primary image, then each extension
# whose jar follows on the boot class path.
foreach ($case in @(
    @{ Name = 'Hello (core-oj, core-libart, core-icu4j)'; Args = $helloArgs; Marker = 'Hello from ART on Windows';
       Spaces = 'boot.art boot-core-libart.art boot-core-icu4j.art' },
    @{ Name = 'framework.jar on the boot class path'; Marker = 'FrameworkSmoke all passed';
       Args = @("-Xbootclasspath:$bcpFrameworkOnly", '-Xint', '-Xusejit:false', '-cp', (NoDrive "$Dir/framework-tests.dex"), 'gesher.FrameworkSmoke');
       Spaces = 'boot.art boot-core-libart.art boot-core-icu4j.art boot-framework.art' },
    @{ Name = 'lifecycle.apk (framework.jar, gesher-app.jar)'; Args = $lifecycleArgs; Marker = 'gesher-app: done';
       Spaces = 'boot.art boot-core-libart.art boot-core-icu4j.art boot-framework.art boot-gesher-app.art' })) {
  $r = Startup $image $case.Args $case.Marker $true
  $count = ($case.Spaces -split ' ').Count
  $loaded = "gesher: boot image loaded: $count spaces ($($case.Spaces))"
  if ($r.Exit -eq 0 -and $null -ne $r.At -and $r.Out.Contains($loaded) -and $r.Out -notmatch 'imageless') {
    Write-Host "PASS  boot image: $($case.Name) runs on it, $count spaces ($($case.Spaces)), no imageless fallback"
  } else {
    Write-Host ('FAIL  boot image: {0} (exit 0x{1:X8}; expected "{2}")' -f $case.Name, $r.Exit, $loaded)
    ($r.Out -split "`n" | Select-Object -Last 40) | Out-Host
    $script:failed++
  }
}

# Startup times, imageless and with the image, alternating, median of 3: Hello
# to its line, and lifecycle.apk (gesher-app's single-process driver) to
# LifecycleActivity.onCreate, both from the process start.
$times = @{}
foreach ($k in 'hello-none', 'hello-image', 'app-none', 'app-image', 'app-none-total', 'app-image-total') { $times[$k] = @() }
$ok = $true
for ($i = 0; $i -lt 3; $i++) {
  foreach ($mode in 'none', 'image') {
    $img = if ($mode -eq 'image') { $image } else { $noImage }
    $h = Startup $img $helloArgs 'Hello from ART on Windows' ($mode -eq 'image')
    $a = Startup $img $lifecycleArgs 'LifecycleActivity onCreate' ($mode -eq 'image')
    foreach ($r in $h, $a) {
      if ($r.Exit -ne 0 -or $null -eq $r.At -or (($mode -eq 'none') -ne ($r.Out -match 'imageless'))) {
        $ok = $false
        Write-Host ('timing run ({0}) failed: exit 0x{1:X8}' -f $mode, $r.Exit)
        ($r.Out -split "`n" | Select-Object -Last 20) | Out-Host
      }
    }
    $times["hello-$mode"] += $h.At
    $times["app-$mode"] += $a.At
    $times["app-$mode-total"] += $a.Total
  }
}
if ($ok) {
  $m = @{}
  foreach ($k in $times.Keys) { $m[$k] = [int](Median $times[$k]) }
  $line = ("INFO  boot image: startup (median of 3, from the process start): Hello {0} ms imageless, {1} ms with the image; " +
           "lifecycle.apk to Activity.onCreate {2} ms imageless, {3} ms with the image (to exit: {4} ms, {5} ms)") -f
    $m['hello-none'], $m['hello-image'], $m['app-none'], $m['app-image'], $m['app-none-total'], $m['app-image-total']
  Write-Host $line
  if ($m['app-image'] -lt $m['app-none'] -and $m['hello-image'] -lt $m['hello-none']) {
    Write-Host 'PASS  boot image: Hello and lifecycle.apk start faster with it than imageless'
  } else {
    Write-Host 'FAIL  boot image: startup with the image is not faster than imageless'
    $script:failed++
  }
} else {
  Write-Host 'FAIL  boot image: the startup timing runs failed'
  $script:failed++
}
