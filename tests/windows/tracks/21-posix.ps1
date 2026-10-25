# gesher-base's POSIX layer and libcore on it (docs/posix.md): ART's own gtests
# for libartbase and libdexfile.
#
# Dot-sourced by run-tests.ps1 (and by posix.yml on its own, with
# $PosixGtestsOnly set), so it adds to $failed in the caller's scope.

# --- ART's gtests ------------------------------------------------------------------
# art_libartbase_tests and art_libdexfile_tests, built by .github/workflows/posix.yml
# into $Dir/art-gtests with the test jars they read. Every test runs except those
# listed below, each excluded for the reason given in docs/posix.md.

function NoDriveP([string] $p) { ($p -replace '\\', '/') -replace '^[A-Za-z]:', '' }

# ART's host test setup looks for the build tree above the executable
# (art::testing::GetAndroidBuildTop: a "linux-x86" directory), the boot class
# path under $ANDROID_ROOT/apex/com.android.art/javalib, and makes its
# ANDROID_DATA directories under $TMPDIR.
$artGtests = Join-Path $Dir 'art-gtests'
$artTop = Join-Path ([IO.Path]::GetTempPath()) "gesher-art-gtests-$PID"
$artHostOut = Join-Path $artTop 'out/host/linux-x86'
$artBin = Join-Path $artHostOut 'nativetest64/art'
$artTmp = Join-Path $artTop 'tmp'

function Invoke-ArtGtest([string] $Name, [string[]] $Exclude = @(), [int] $TimeoutSec = 900) {
  $exe = Join-Path $artBin "$Name.exe"
  if (-not (Test-Path $exe)) {
    Write-Host "FAIL  $Name (missing: $exe)"
    return $false
  }
  $arguments = @('--gtest_color=no')
  if ($Exclude) { $arguments += "--gtest_filter=-$($Exclude -join ':')" }
  $out = New-TemporaryFile
  $err = New-TemporaryFile
  $p = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $artBin `
    -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow -PassThru
  $null = $p.Handle  # keeps ExitCode readable after the process ends
  $timedOut = -not $p.WaitForExit($TimeoutSec * 1000)
  if ($timedOut) { $p | Stop-Process -Force -ErrorAction SilentlyContinue }
  $p.WaitForExit()
  $stdout = "$(Get-Content -Raw $out)"
  $stderr = "$(Get-Content -Raw $err)"
  $text = $stdout + $stderr
  Remove-Item $out, $err -ErrorAction SilentlyContinue
  $passed = if ($text -match '\[  PASSED  \] (\d+) test') { [int] $Matches[1] } else { 0 }
  $failedTests = @($text -split "`r?`n" | Where-Object { $_ -match '^\[  FAILED  \] [^ ]+\.[^ ]+' } |
    ForEach-Object { ($_ -replace '^\[  FAILED  \] ', '') -replace ',.*$', '' } | Sort-Object -Unique)
  $excluded = if ($Exclude) { " ($($Exclude.Count) excluded, docs/posix.md)" } else { '' }
  if (-not $timedOut -and $p.ExitCode -eq 0 -and $passed -gt 0 -and -not $failedTests) {
    Write-Host "PASS  $Name`: $passed tests passed$excluded"
    return $true
  }
  # The test a timeout or crash stopped in: the last one started and not ended.
  $last = @($stdout -split "`r?`n" | Where-Object { $_ -match '^\[ RUN      \] ' }) | Select-Object -Last 1
  $ended = $last -and $stdout -match ('\[       OK \] ' + [regex]::Escape(($last -replace '^\[ RUN      \] ', '')) + '|\[  FAILED  \] ' + [regex]::Escape(($last -replace '^\[ RUN      \] ', '')))
  $why = if ($timedOut) { "timed out after $TimeoutSec s" } else { 'exit 0x{0:X8}' -f $p.ExitCode }
  if ($last -and -not $ended) { $why += " in $($last -replace '^\[ RUN      \] ', '')" }
  Write-Host "FAIL  $Name`: $why, $passed passed, $($failedTests.Count) failed$excluded"
  foreach ($t in $failedTests) { Write-Host "        $t" }
  $lines = $text -split "`r?`n"
  $lines | Select-String -Pattern 'Failure|error:|Expected|Which is|Value of|Actual|Check failed' -Context 2, 4 |
    Select-Object -First 80 | Out-Host
  Write-Host '        ... (its error log lines, known noise left out)'
  $stderr -split "`r?`n" | Where-Object { $_ -match ' [EF] ' -and $_ -notmatch 'logging\.cc:197|SetCloseOnExec|realpath is unsupported' } |
    Select-Object -First 40 | Out-Host
  Write-Host '        ... (the end of its standard output)'
  $stdout -split "`r?`n" | Select-Object -Last 25 | Out-Host
  Write-Host '        ... (the end of its standard error)'
  $stderr -split "`r?`n" | Select-Object -Last 25 | Out-Host
  return $false
}

if (Test-Path $artGtests) {
  New-Item -ItemType Directory -Force $artBin, $artTmp, (Join-Path $artHostOut 'apex/com.android.art/javalib') | Out-Null
  Copy-Item "$artGtests/*" $artBin
  Copy-Item "$Dir/core-oj.jar" (Join-Path $artHostOut 'apex/com.android.art/javalib')
  $saved = @{}
  foreach ($v in 'ANDROID_ROOT', 'ANDROID_DATA', 'ANDROID_ART_ROOT', 'ANDROID_I18N_ROOT', 'ANDROID_TZDATA_ROOT',
                 'ANDROID_BUILD_TOP', 'ANDROID_HOST_OUT', 'TMPDIR') {
    $saved[$v] = [Environment]::GetEnvironmentVariable($v)
    [Environment]::SetEnvironmentVariable($v, $null)
  }
  $env:ANDROID_ROOT = NoDriveP $artHostOut
  $env:TMPDIR = NoDriveP $artTmp
  try {
    if (-not (Invoke-ArtGtest 'art_libartbase_tests' -TimeoutSec 600)) { $failed++ }
    if (-not (Invoke-ArtGtest 'art_libdexfile_tests' -Exclude @(
        # VerifierDeps is smali, which the build does not assemble.
        'ArtDexFileLoaderTest.GetMultiDexChecksumsDexFile',
        # DexFileLoader does not resolve symbolic links on Windows (no realpath).
        'ArtDexFileLoaderTest.GetDexCanonicalLocation'))) { $failed++ }
  } finally {
    foreach ($v in $saved.Keys) { [Environment]::SetEnvironmentVariable($v, $saved[$v]) }
    Remove-Item -Recurse -Force $artTop -ErrorAction SilentlyContinue
  }
} else {
  Write-Host "FAIL  ART gtests (missing: $artGtests)"
  $failed++
}

if ($PosixGtestsOnly) { return }

# --- gesher-base ------------------------------------------------------------------
# Paths (other drives, long and non-ASCII names, rename, links, fopen), /proc,
# /etc/timezone, inotify and the process model (probes/posix/posix_probe.cc).
# It works in a directory of its own under the current one (the workspace's
# drive), and on TEMP's drive as /mnt/<drive> when that is another one.
Check 'posix probe: paths, /proc, /etc/timezone, inotify, processes' { & "$Dir/posix_probe.exe" } @('all passed')

# A class path on another drive: /mnt/<drive>/... (ADR 0045). The tests run on
# the workspace's drive; TEMP is on another one on GitHub's runners.
$tempDrive = [IO.Path]::GetTempPath().Substring(0, 1)
$here = (Get-Location).Drive.Name
if ($tempDrive -ine $here) {
  $otherDex = Join-Path ([IO.Path]::GetTempPath()) "gesher-hello-$PID.dex"
  Copy-Item "$Dir/hello.dex" $otherDex
  $mnt = "/mnt/$($tempDrive.ToLower())" + (($otherDex.Substring(2)) -replace '\\', '/')
  Check "dalvikvm: a class path entry on another drive ($mnt)" {
    & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
      -Xint '-Xusejit:false' -cp "$(NoDrive "$Dir/libcore-tests.dex"):$mnt" gesher.Hello
  } @('Hello from ART on Windows')
  Remove-Item $otherDex -ErrorAction SilentlyContinue
} else {
  Write-Host "INFO  no second drive: the class path on another drive is not checked"
}

# --- time zones ---------------------------------------------------------------------
# TimeZone.getDefault() follows Windows' zone; java.util, java.time and
# android.icu agree with Windows' own offsets, at every transition in
# 2025-2027 (tests/dex-libcore/TimeZoneTest.java). Changes the machine's zone
# (GitHub's runners allow it) and restores it.

# Instants to check in a .NET zone: around each offset change, and midway.
function ZoneInstants([TimeZoneInfo] $tz) {
  $instants = @()
  $start = [DateTimeOffset]::new(2025, 1, 1, 0, 0, 0, [TimeSpan]::Zero).ToUnixTimeMilliseconds()
  $end = [DateTimeOffset]::new(2028, 1, 1, 0, 0, 0, [TimeSpan]::Zero).ToUnixTimeMilliseconds()
  $hour = 3600000
  $offset = { param([long] $t) $tz.GetUtcOffset([DateTimeOffset]::FromUnixTimeMilliseconds($t)).TotalMilliseconds }
  $prev = & $offset $start
  for ($t = $start + $hour; $t -lt $end; $t += $hour) {
    $now = & $offset $t
    if ($now -ne $prev) {
      $lo = $t - $hour; $hi = $t  # the change is in (lo, hi]
      while ($hi - $lo -gt 1000) {
        $mid = [long](($lo + $hi) / 2); $mid -= $mid % 1000
        if ((& $offset $mid) -eq $prev) { $lo = $mid } else { $hi = $mid }
      }
      $instants += @(($hi - 1000), $hi, ($hi + 1000))  # (',' binds tighter than '-')
      $prev = $now
    }
  }
  $instants += @($start, [long](($start + $end) / 2), $end)
  return $instants
}

$zones = [ordered]@{
  'Eastern Standard Time' = 'America/New_York'
  'Israel Standard Time' = 'Asia/Jerusalem'
  'AUS Eastern Standard Time' = 'Australia/Sydney'
  'UTC' = 'Etc/UTC'
}
$originalZone = (Get-TimeZone).Id
try {
  foreach ($windowsId in $zones.Keys) {
    try { Set-TimeZone -Id $windowsId -ErrorAction Stop } catch {
      Write-Host "FAIL  time zone ${windowsId}: cannot set it ($_)"
      $failed++
      continue
    }
    $tz = [TimeZoneInfo]::FindSystemTimeZoneById($windowsId)
    $instants = ZoneInstants $tz
    $expected = $zones[$windowsId]
    $probeTz = (& "$Dir/posix_probe.exe" --timezone | Out-String).Trim()
    $out = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
      -Xint '-Xusejit:false' -cp (NoDrive "$Dir/libcore-tests.dex") gesher.TimeZoneTest $expected @instants 2>&1 | Out-String
    $code = $LASTEXITCODE
    $mismatch = @()
    foreach ($m in [regex]::Matches($out, 'offset (-?\d+) (-?\d+)')) {
      $t = [long]$m.Groups[1].Value
      $windows = $tz.GetUtcOffset([DateTimeOffset]::FromUnixTimeMilliseconds($t)).TotalMilliseconds
      if ([long]$m.Groups[2].Value -ne $windows) { $mismatch += "$t`: java $($m.Groups[2].Value), Windows $windows" }
    }
    $transitions = ($instants.Count - 3) / 3
    if ($code -eq 0 -and $out.Contains('TimeZoneTest all passed') -and $probeTz -eq $expected -and -not $mismatch -and
        [regex]::Matches($out, 'offset ').Count -eq $instants.Count) {
      Write-Host "PASS  time zone ${windowsId}: /etc/timezone and TimeZone.getDefault() are $expected; java.util, java.time and android.icu match Windows at $transitions transitions in 2025-2027"
    } else {
      Write-Host "FAIL  time zone ${windowsId}: expected $expected, /etc/timezone '$probeTz' (exit $code)"
      $mismatch | Select-Object -First 10 | Out-Host
      Write-Host $out
      $failed++
    }
  }
} finally {
  Set-TimeZone -Id $originalZone
}

# --- inotify from Java ------------------------------------------------------------
# java.nio.file.WatchService (LinuxWatchService) and android.os.FileObserver
# (framework.jar) over gesher-base's inotify.
$watchDir = Join-Path $root "data/posix-watch-$PID"
Check 'dalvikvm: java.nio.file.WatchService over inotify' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/libcore-tests.dex") gesher.WatchServiceTest (NoDrive "$watchDir/ws")
} @('WatchServiceTest all passed')
$bcpFramework = (('core-oj', 'core-libart', 'core-icu4j', 'framework') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
Check 'dalvikvm: android.os.FileObserver over inotify (framework.jar)' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpFramework" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/posix-tests.dex") gesher.FileObserverTest (NoDrive "$watchDir/fo")
} @('FileObserverTest all passed')
Remove-Item -Recurse -Force $watchDir -ErrorAction SilentlyContinue
