# Android's compilation model for apps and the framework (docs/compiled-code.md,
# ADR 0054). Dot-sourced by run-tests.ps1: NoDrive, $Dir, $root, $bcp and
# $failed are its; 60-boot-image.ps1 ran before (the image in
# $root/system/framework/x86_64).
#
#   1. The framework's boot image is compiled with AOSP's boot image profiles
#      (speed-profile): its size and startup against a `speed` image compiled
#      here, median of 3 as 60-boot-image.ps1 times it.
#   2. One process (com.gesher.app.Main): dex2oat compiles lifecycle.apk as at
#      install (verify); the app runs with the JIT, which saves its profile;
#      profman merges it; dex2oat compiles speed-profile with an app image; the
#      app runs from it (ART's own log lines, GESHER_LOG_EXECUTION).
#   3. The Gesher device: the system server compiles at install, app
#      processes run with the JIT and save their profiles, background dexopt
#      compiles speed-profile with an app image once the device is idle, and
#      the next launch runs from it.

$aotRoot = Join-Path $root 'aot2'
New-Item -ItemType Directory -Force $aotRoot | Out-Null
$aotImage = "-Ximage:$(NoDrive "$root/system")/framework/boot.art"
$aotBcpApp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'

# Runs a program with a time limit; returns exit code, output (stdout + stderr)
# and the time it took.
function Aot2Run([string] $Exe, [string[]] $Arguments, [hashtable] $Env = @{}, [int] $Seconds = 300) {
  $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
  foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  foreach ($k in $Env.Keys) { $psi.Environment[$k] = $Env[$k] }
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $p = [Diagnostics.Process]::Start($psi)
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEndAsync()
  $timedOut = -not $p.WaitForExit($Seconds * 1000)
  if ($timedOut) { try { $p.Kill($true) } catch {} ; $null = $p.WaitForExit(30000) }
  $null = $stdout.Wait(30000)
  $null = $stderr.Wait(30000)
  $total = $watch.Elapsed.TotalMilliseconds
  $out = $(if ($stdout.IsCompleted) { $stdout.Result } else { '' }) + "`n" + $(if ($stderr.IsCompleted) { $stderr.Result } else { '' })
  if ($timedOut) { $out += "`n(timed out after $Seconds s)" }
  [pscustomobject]@{ Exit = $(if ($timedOut) { -1 } else { $p.ExitCode }); Out = $out; Ms = $total }
}

# Startup to a marker on stdout, from the process start (60-boot-image.ps1's way).
function Aot2Startup([string[]] $Arguments, [string] $Marker, [hashtable] $Env = @{}) {
  $psi = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
  foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  foreach ($k in $Env.Keys) { $psi.Environment[$k] = $Env[$k] }
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
  [pscustomobject]@{ At = $at; Total = $watch.Elapsed.TotalMilliseconds; Exit = $p.ExitCode; Out = (($lines -join "`n") + "`n" + $stderr.Result) }
}

function Aot2Median([double[]] $xs) { $s = @($xs | Sort-Object); $s[[int][math]::Floor($s.Count / 2)] }

function Aot2Fail([string] $Name, [string] $Out, [int] $Last = 60) {
  Write-Host "FAIL  aot2: $Name"
  ($Out -split "`n" | Select-Object -Last $Last) | Out-Host
  $script:failed++
}

# dex2oat for an app, as Dexopt.compile runs it (com.gesher.server).
function Aot2Dex2oat([string] $Apk, [string] $Odex, [string] $Filter, [string[]] $Extra = @()) {
  New-Item -ItemType Directory -Force (Split-Path $Odex) | Out-Null
  $a = @("--dex-file=$(NoDrive $Apk)", "--dex-location=$(NoDrive $Apk)", "--oat-file=$(NoDrive $Odex)",
    '--instruction-set=x86_64', "--compiler-filter=$Filter", '--class-loader-context=PCL[]',
    "--boot-image=$(NoDrive "$root/system")/framework/boot.art",
    '--runtime-arg', "-Xbootclasspath:$aotBcpApp", '--runtime-arg', '-Xms64m', '--runtime-arg', '-Xmx512m',
    '--runtime-arg', '-XX:LargeObjectSpace=map', '--no-watch-dog', '-j4') + $Extra
  Aot2Run "$Dir/dex2oat.exe" $a @{} 600
}

function Aot2Size([string] $Path) { if (Test-Path $Path) { (Get-Item $Path).Length } else { 0 } }

$aotLauncher = @("-Xbootclasspath:$aotBcpApp", $aotImage, '-Xint', '-Xusejit:false', 'com.gesher.app.Launcher')
# A device the checks before left running would compile its apps in the
# background (dexopt) while the timings below run: it goes first.
$null = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'shutdown') @{} 120

# --- 1. The framework's boot image with a profile ---------------------------------
try {

$imageDir = Join-Path $root 'system/framework/x86_64'
$afterOat = Aot2Size (Join-Path $imageDir 'boot-framework.oat')
$afterAll = (Get-ChildItem $imageDir -File | Measure-Object Length -Sum).Sum
$afterOats = (Get-ChildItem $imageDir -Filter *.oat | Measure-Object Length -Sum).Sum
# Before: what boot-image.yml compiled until this track, every method of every
# jar (`speed`), compiled here by the same script.
$before = Join-Path $aotRoot 'before'
$compile = Join-Path $PSScriptRoot '../../../tools/boot-image/compile.ps1'
$beforeOk = $false
if (Test-Path $compile) {
  $out = pwsh -NoProfile -File $compile -Dist $Dir -Out $before -Filter speed -ExtensionFilter speed -TimeoutMinutes 15 2>&1 | Out-String
  $beforeOk = $LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $before 'framework/x86_64/boot-framework.oat'))
  if (-not $beforeOk) { Aot2Fail 'compiling the speed boot image to compare with' $out }
} else {
  Aot2Fail "no tools/boot-image/compile.ps1 ($compile)" ''
}
if ($beforeOk) {
  $beforeDir = Join-Path $before 'framework/x86_64'
  $beforeOat = Aot2Size (Join-Path $beforeDir 'boot-framework.oat')
  $beforeAll = (Get-ChildItem $beforeDir -File | Measure-Object Length -Sum).Sum
  $beforeOats = (Get-ChildItem $beforeDir -Filter *.oat | Measure-Object Length -Sum).Sum
  Write-Host ("INFO  aot2: boot image sizes: boot-framework.oat {0:N1} MB with the profile, {1:N1} MB speed; all .oat files {2:N1} / {3:N1} MB; the whole image {4:N1} / {5:N1} MB" -f
    ($afterOat / 1MB), ($beforeOat / 1MB), ($afterOats / 1MB), ($beforeOats / 1MB), ($afterAll / 1MB), ($beforeAll / 1MB))
  if ($afterOat -gt 0 -and $afterOat -lt $beforeOat / 2) {
    Write-Host ("PASS  aot2: framework.jar compiled with the boot image profile (speed-profile): boot-framework.oat {0:N1} MB, was {1:N1} MB (speed); the image {2:N1} MB, was {3:N1} MB" -f
      ($afterOat / 1MB), ($beforeOat / 1MB), ($afterAll / 1MB), ($beforeAll / 1MB))
  } else {
    Write-Host ("FAIL  aot2: boot-framework.oat is not smaller with the profile ({0:N1} MB vs {1:N1} MB speed)" -f ($afterOat / 1MB), ($beforeOat / 1MB))
    $failed++
  }

  # Startup, median of 3, alternating: lifecycle.apk to LifecycleActivity.onCreate
  # under the interpreter (60-boot-image.ps1's runs) and with the JIT (the
  # Gesher device's app processes), on each image.
  $images = [ordered]@{ profile = $aotImage; speed = "-Ximage:$(NoDrive $before)/framework/boot.art" }
  $modes = [ordered]@{ interpreter = @('-Xint', '-Xusejit:false'); jit = @('-Xusejit:true') }
  $t = @{}
  $ok = $true
  for ($i = 0; $i -lt 3; $i++) {
    foreach ($m in $modes.Keys) {
      foreach ($img in $images.Keys) {
        $r = Aot2Startup (@("-Xbootclasspath:$aotBcpApp", $images[$img]) + $modes[$m] + @('com.gesher.app.Main', (NoDrive "$Dir/lifecycle.apk"))) 'LifecycleActivity onCreate' @{ GESHER_REQUIRE_BOOT_IMAGE = '1' }
        if ($r.Exit -ne 0 -or $null -eq $r.At -or $r.Out -notmatch 'boot image loaded: 5 spaces') {
          $ok = $false
          Write-Host ('timing run ({0}, {1} image) failed: exit 0x{2:X8}' -f $m, $img, $r.Exit)
          ($r.Out -split "`n" | Select-Object -Last 20) | Out-Host
          continue
        }
        $t["$m-$img"] += @($r.At)
        $t["$m-$img-total"] += @($r.Total)
      }
    }
  }
  if ($ok) {
    $md = @{}
    foreach ($k in $t.Keys) { $md[$k] = [int](Aot2Median $t[$k]) }
    Write-Host ("INFO  aot2: startup, lifecycle.apk to Activity.onCreate (median of 3): interpreter {0} ms with the profile image, {1} ms with the speed image; JIT {2} / {3} ms (to exit: {4} / {5}, {6} / {7} ms)" -f
      $md['interpreter-profile'], $md['interpreter-speed'], $md['jit-profile'], $md['jit-speed'],
      $md['interpreter-profile-total'], $md['interpreter-speed-total'], $md['jit-profile-total'], $md['jit-speed-total'])
    # No worse: within 10% (and 30 ms) of the speed image, for timer noise.
    $worse = @()
    foreach ($m in $modes.Keys) {
      if ($md["$m-profile"] -gt [math]::Max($md["$m-speed"] * 1.10, $md["$m-speed"] + 30)) { $worse += $m }
    }
    if (-not $worse) {
      Write-Host ("PASS  aot2: startup with the profile image no worse than with the speed image (interpreter {0} vs {1} ms, JIT {2} vs {3} ms)" -f
        $md['interpreter-profile'], $md['interpreter-speed'], $md['jit-profile'], $md['jit-speed'])
    } else {
      Write-Host "FAIL  aot2: startup with the profile image is worse ($($worse -join ', '))"
      $failed++
    }
  } else {
    Write-Host 'FAIL  aot2: the startup timing runs failed'
    $failed++
  }
  Remove-Item -Recurse -Force $before -ErrorAction SilentlyContinue
}

} catch { Aot2Fail "the boot image checks threw: $($_.Exception.Message)" "$($_.ScriptStackTrace)" }

# --- 2. One process: install-time compile, JIT profile, speed-profile + app image -
try {

$single = Join-Path $aotRoot 'single'
New-Item -ItemType Directory -Force $single | Out-Null
$apk = Join-Path $single 'lifecycle.apk'
Copy-Item "$Dir/lifecycle.apk" $apk
$odex = Join-Path $single 'oat/x86_64/lifecycle.odex'
$pkg = 'com.gesher.lifecycle'
$curProfile = Join-Path $root "data/misc/profiles/cur/0/$pkg/primary.prof"
$refProfile = Join-Path $root "data/misc/profiles/ref/$pkg/primary.prof"
foreach ($f in $curProfile, $refProfile) {
  New-Item -ItemType Directory -Force (Split-Path $f) | Out-Null
  Set-Content -NoNewline -Path $f -Value ''
}
$appArgs = @("-Xbootclasspath:$aotBcpApp", $aotImage, '-Xusejit:true', '-Xjitsaveprofilinginfo', 'com.gesher.app.Main', (NoDrive $apk))
$logEnv = @{ GESHER_LOG_EXECUTION = '1' }
$expectApp = @('LifecycleActivity onCreate', 'gesher-app: done')

# The app's methods at exit: classes, methods, with AOT code, interpreted.
function Aot2Methods([string] $Out) {
  if ($Out -match 'gesher: app methods at exit: (\d+) classes, (\d+) methods, (\d+) with AOT code, (\d+) entered the interpreter or the JIT \((\d+) of them with AOT code\)') {
    return [pscustomobject]@{ Classes = [int]$Matches[1]; Methods = [int]$Matches[2]; Aot = [int]$Matches[3]; Interpreted = [int]$Matches[4]; InterpretedAot = [int]$Matches[5] }
  }
  return $null
}

# Before any compile: no oat file, the app runs from its APK.
$r0 = Aot2Run "$Dir/dalvikvm.exe" $appArgs $logEnv
$m0 = Aot2Methods $r0.Out

$r = Aot2Dex2oat $apk $odex 'verify' @('--compilation-reason=install')
$verifyOk = $r.Exit -eq 0 -and (Test-Path $odex) -and (Test-Path ($odex -replace '\.odex$', '.vdex'))
if ($verifyOk) {
  Write-Host ("PASS  aot2: dex2oat compiles lifecycle.apk as at install (verify): lifecycle.odex {0:N0} bytes, lifecycle.vdex {1:N0} bytes in {2:N1} s" -f
    (Aot2Size $odex), (Aot2Size ($odex -replace '\.odex$', '.vdex')), ($r.Ms / 1000))
} else {
  Aot2Fail 'dex2oat lifecycle.apk (verify)' $r.Out
}

if ($verifyOk) {
  $r1 = Aot2Run "$Dir/dalvikvm.exe" $appArgs $logEnv
  $m1 = Aot2Methods $r1.Out
  $loc = [regex]::Escape((NoDrive $apk))
  if ($r1.Exit -eq 0 -and -not ($expectApp | Where-Object { -not $r1.Out.Contains($_) }) -and
      $r1.Out -match "gesher: app oat: ${loc}: \S*lifecycle\.odex \(status [^,]*, filter verify") {
    Write-Host "PASS  aot2: lifecycle.apk runs from its verify oat file, with the JIT ($(($r1.Out -split "`n" | Where-Object { $_ -match 'gesher: app oat: ' } | Select-Object -First 1) -replace '^.*gesher: ', ''))"
  } else {
    Aot2Fail 'lifecycle.apk on its verify oat file, with the JIT' $r1.Out
  }
  $profileSize = Aot2Size $curProfile
  if ($profileSize -gt 0 -and $r1.Out.Contains('gesher: JIT profile saved at exit')) {
    Write-Host "PASS  aot2: the JIT's profile saver wrote lifecycle.apk's profile ($profileSize bytes in data/misc/profiles/cur/0/$pkg/primary.prof)"
  } else {
    Aot2Fail "no JIT profile in $curProfile ($profileSize bytes)" $r1.Out
  }

  # Background dexopt, by hand: profman merges, dex2oat compiles speed-profile.
  $pm = Aot2Run "$Dir/profman.exe" @("--profile-file=$(NoDrive $curProfile)", "--reference-profile-file=$(NoDrive $refProfile)",
    "--apk=$(NoDrive $apk)", "--dex-location=$(NoDrive $apk)", '--force-merge-and-analyze')
  $dump = Aot2Run "$Dir/profman.exe" @("--dump-only", "--profile-file=$(NoDrive $refProfile)", "--apk=$(NoDrive $apk)", "--dex-location=$(NoDrive $apk)")
  $hot = ([regex]::Matches($dump.Out, 'hot methods: ([0-9 ,]*)')).Count
  if ($pm.Exit -eq 1 -and (Aot2Size $refProfile) -gt 0) {
    Write-Host ("PASS  aot2: profman merges the JIT profile into the reference profile ({0:N0} bytes) and asks for a compile (exit 1)" -f (Aot2Size $refProfile))
    ($dump.Out -split "`n" | Select-Object -First 12) | Out-Host
  } else {
    Aot2Fail ("profman merge (exit {0}, reference profile {1} bytes)" -f $pm.Exit, (Aot2Size $refProfile)) ($pm.Out + "`n" + $dump.Out)
  }
  $art = $odex -replace '\.odex$', '.art'
  $r = Aot2Dex2oat $apk $odex 'speed-profile' @('--compilation-reason=bg-dexopt', "--profile-file=$(NoDrive $refProfile)",
    "--app-image-file=$(NoDrive $art)", '--image-format=lz4')
  $spOk = $r.Exit -eq 0 -and (Test-Path $art)
  if ($spOk) {
    Write-Host ("PASS  aot2: dex2oat compiles lifecycle.apk speed-profile with an app image: .odex {0:N0}, .vdex {1:N0}, .art {2:N0} bytes in {3:N1} s" -f
      (Aot2Size $odex), (Aot2Size ($odex -replace '\.odex$', '.vdex')), (Aot2Size $art), ($r.Ms / 1000))
  } else {
    Aot2Fail 'dex2oat lifecycle.apk (speed-profile, app image)' $r.Out
  }
  if ($spOk) {
    $r2 = Aot2Run "$Dir/dalvikvm.exe" $appArgs $logEnv
    $m2 = Aot2Methods $r2.Out
    $imageRe = 'gesher: app image loaded: .*lifecycle\.art: (\d+) classes, (\d+) methods, (\d+) with compiled code .*speed-profile'
    $imageLine = ($r2.Out -split "`n" | Where-Object { $_ -match $imageRe } | Select-Object -First 1)
    if ($r2.Exit -eq 0 -and -not ($expectApp | Where-Object { -not $r2.Out.Contains($_) }) -and $imageLine -and
        $imageLine -match $imageRe -and [int]$Matches[1] -gt 0 -and [int]$Matches[3] -gt 0 -and $m2) {
      Write-Host "PASS  aot2: lifecycle.apk runs from its app image and speed-profile code ($(($imageLine -replace '^.*gesher: ', '').Trim()))"
    } else {
      Aot2Fail 'lifecycle.apk from its app image (speed-profile)' $r2.Out
    }
    if ($m0 -and $m1 -and $m2) {
      Write-Host ("INFO  aot2: lifecycle.apk's methods that entered the interpreter or the JIT: {0} of {1} from the APK, {2} of {3} with the verify oat file, {4} of {5} with speed-profile code ({6} with AOT code, {7} of those interpreted)" -f
        $m0.Interpreted, $m0.Methods, $m1.Interpreted, $m1.Methods, $m2.Interpreted, $m2.Methods, $m2.Aot, $m2.InterpretedAot)
      if ($m2.Aot -gt 0 -and $m2.InterpretedAot -eq 0 -and $m2.Interpreted -lt $m1.Interpreted) {
        Write-Host "PASS  aot2: with the app image, the app's methods with AOT code ($($m2.Aot)) never entered the interpreter; $($m2.Interpreted) did, $($m1.Interpreted) before"
      } else {
        Write-Host "FAIL  aot2: the app's compiled methods still entered the interpreter ($($m2.InterpretedAot) of $($m2.Aot); $($m2.Interpreted) in all, $($m1.Interpreted) with verify)"
        $failed++
      }
    } else {
      Aot2Fail 'no "gesher: app methods at exit" line (GESHER_LOG_EXECUTION)' ($r2.Out)
    }
    # Startup, median of 3: from the APK, the verify oat file, the app image.
    $st = [ordered]@{ apk = @(); verify = @(); image = @() }
    $bak = Join-Path $aotRoot 'oat-bak'
    for ($i = 0; $i -lt 3; $i++) {
      foreach ($k in 'image', 'apk') {
        if ($k -eq 'apk') { Move-Item (Join-Path $single 'oat') $bak }
        $rs = Aot2Startup $appArgs 'LifecycleActivity onCreate'
        if ($k -eq 'apk') { Move-Item $bak (Join-Path $single 'oat') }
        if ($rs.Exit -eq 0 -and $null -ne $rs.At) { $st[$k] += @($rs.At) }
      }
    }
    if ($st['image'].Count -eq 3 -and $st['apk'].Count -eq 3) {
      Write-Host ("INFO  aot2: lifecycle.apk startup to Activity.onCreate with the JIT (median of 3): {0} ms from the APK, {1} ms with its app image and speed-profile code" -f
        [int](Aot2Median $st['apk']), [int](Aot2Median $st['image']))
    }
  }
}

} catch { Aot2Fail "the one-process checks threw: $($_.Exception.Message)" "$($_.ScriptStackTrace)" }

# --- 3. The Gesher device ----------------------------------------------------------
try {
$aotLogs = Join-Path $root 'logs'
$serverLog = Join-Path $aotLogs 'system_server.log'
$pkgLog = Join-Path $aotLogs "$pkg.log"
# A log the device's processes still append to (shared reading).
function Aot2Read([string] $File) {
  if (-not (Test-Path $File)) { return '' }
  try {
    $fs = [IO.FileStream]::new($File, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite, Delete')
    try { return [IO.StreamReader]::new($fs).ReadToEnd() } finally { $fs.Dispose() }
  } catch {
    return ''
  }
}
function Aot2Tail([string] $File, [long] $From) {
  $text = Aot2Read $File
  if ($From -gt $text.Length) { $From = 0 }
  return $text.Substring([int]$From)
}
function Aot2Len([string] $File) { (Aot2Read $File).Length }

# A device of its own, whose processes log what they run (GESHER_LOG_EXECUTION).
$null = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'shutdown') @{} 120
$devEnv = @{ GESHER_LOG_EXECUTION = '1'; GESHER_APP_WINDOW = 'headless' }
$r = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'uninstall', $pkg) $devEnv 400
$serverFrom = Aot2Len $serverLog
$r = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'install', (NoDrive "$Dir/lifecycle.apk")) $devEnv 400
$server = Aot2Tail $serverLog $serverFrom
$codePath = if ($r.Out -match 'installed com\.gesher\.lifecycle \(version \d+\) in (\S+)') { $Matches[1] } else { $null }
if ($r.Exit -eq 0 -and $codePath -and $server -match "dexopt: ${pkg}: verify \(reason install\) in \d+ ms: base\.odex \d+ base\.vdex \d+") {
  Write-Host "PASS  aot2: the device compiles at install ($(($server -split "`n" | Where-Object { $_ -match "dexopt: ${pkg}: verify" } | Select-Object -First 1) -replace '^.*dexopt: ', 'dexopt: '))"
} else {
  Aot2Fail 'install-time dexopt on the Gesher device' ($r.Out + "`n--- system_server.log`n" + $server)
}

# First launch: the app process runs with the JIT on the verify oat file, and
# saves its profile when it exits.
$pkgFrom = Aot2Len $pkgLog
$serverFrom = Aot2Len $serverLog
$r = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'launch', (NoDrive "$Dir/lifecycle.apk"), '--headless') $devEnv 400
$app = Aot2Tail $pkgLog $pkgFrom
$devCur = Join-Path $root "data/misc/profiles/cur/0/$pkg/primary.prof"
if ($r.Exit -eq 0 -and $r.Out.Contains('gesher-app: done') -and $app -match 'gesher: app oat: \S*/base\.apk: \S*base\.odex \(status [^,]*, filter verify' -and
    $app.Contains('gesher: JIT profile saved at exit')) {
  Write-Host "PASS  aot2: on the device, lifecycle.apk's process runs with the JIT on its install-time code and saves its profile ($(Aot2Size $devCur) bytes)"
} else {
  Aot2Fail 'first launch on the device (verify oat file, JIT profile)' ($r.Out + "`n--- $pkg.log`n" + $app)
}

# Background dexopt: once the device is idle, the profile is merged and the app
# compiled speed-profile with an app image.
$bg = $null
for ($i = 0; $i -lt 180 -and -not $bg; $i++) {
  Start-Sleep 1
  $server = Aot2Tail $serverLog $serverFrom
  $bg = ($server -split "`n" | Where-Object { $_ -match "dexopt: ${pkg}: speed-profile \(reason bg-dexopt\) in \d+ ms: base\.odex \d+ base\.vdex \d+ base\.art \d+" } | Select-Object -First 1)
}
if ($bg) {
  $merge = ($server -split "`n" | Where-Object { $_ -match "dexopt: ${pkg}: profman merged" } | Select-Object -First 1) -replace '^.*dexopt: ', 'dexopt: '
  Write-Host "PASS  aot2: background dexopt with the JIT profile once the device is idle ($merge; $(($bg -replace '^.*dexopt: ', 'dexopt: ').Trim()))"
} else {
  Aot2Fail 'background dexopt on the device (speed-profile, app image)' ("--- system_server.log`n" + $server + "`n--- dexopt.log`n" + (Aot2Tail (Join-Path $aotLogs 'dexopt.log') 0))
}

# Second launch: from the app image and the speed-profile code.
$pkgFrom = Aot2Len $pkgLog
$r = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'launch', (NoDrive "$Dir/lifecycle.apk"), '--headless') $devEnv 400
$app = Aot2Tail $pkgLog $pkgFrom
$imageRe = 'gesher: app image loaded: \S*base\.art: (\d+) classes, (\d+) methods, (\d+) with compiled code .*speed-profile'
$imageLine = ($app -split "`n" | Where-Object { $_ -match $imageRe } | Select-Object -First 1)
$md = Aot2Methods $app
if ($r.Exit -eq 0 -and $r.Out.Contains('gesher-app: done') -and $imageLine -and $imageLine -match $imageRe -and
    [int]$Matches[3] -gt 0 -and $md -and $md.InterpretedAot -eq 0) {
  Write-Host "PASS  aot2: on the device, the next launch runs from the app image ($(($imageLine -replace '^.*gesher: ', '').Trim()); app methods: $($md.Aot) with AOT code, none of them interpreted; $($md.Interpreted) of $($md.Methods) entered the interpreter or the JIT)"
} else {
  Aot2Fail 'second launch on the device (app image)' ($r.Out + "`n--- $pkg.log`n" + $app)
}
} catch { Aot2Fail "the device checks threw: $($_.Exception.Message)" "$($_.ScriptStackTrace)" }
$null = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'shutdown') @{} 120
