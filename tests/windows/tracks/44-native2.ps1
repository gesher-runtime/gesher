# native2 track (docs/native-libs.md, docs/posix.md, ADR 0057): the bionic
# shim's additions, dlopen of the platform's libraries, popular NDK libraries'
# own tests, and the file-system leftovers, for x86_64 and arm64-v8a.
# Dot-sourced by run-tests.ps1, so Check, NoDrive, $Dir, $root, $bcp and
# $failed are in scope. The programs come from .github/workflows/native2.yml
# (the "native2" artifact, in $Dir/native2).

$n2 = Join-Path $Dir 'native2'
if (-not (Test-Path "$n2/native2-test.dex")) {
  Write-Host "FAIL  native2: the native2 artifact is missing ($n2)"
  $failed++
  return
}
$n2 = (Resolve-Path $n2).Path
$n2Dir = (Resolve-Path $Dir).Path
$arm64FpcrEvidence = [Collections.Generic.List[object]]::new()

# Runs a program with a time limit (a translated or broken run must never
# hang the job), in $WorkDir (by default the current directory: $bcp's paths
# are relative to it), with extra environment variables; prints its
# output and sets $LASTEXITCODE (124 on a timeout).
function N2Run([string] $Exe, [string[]] $Arguments, [string] $WorkDir = (Get-Location).Path,
               [hashtable] $Environment = @{}, [int] $Seconds = 600) {
  $out = Join-Path $env:RUNNER_TEMP "n2-$([guid]::NewGuid()).out"
  $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
  foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
  $psi.WorkingDirectory = $WorkDir
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  foreach ($k in $Environment.Keys) { $psi.Environment[$k] = $Environment[$k] }
  $p = [Diagnostics.Process]::Start($psi)
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEndAsync()
  if (-not $p.WaitForExit($Seconds * 1000)) {
    $p.Kill($true)
    $p.WaitForExit()
    Write-Output "TIMEOUT after $Seconds s: $Exe $($Arguments -join ' ')"
    $global:LASTEXITCODE = 124
  } else {
    $p.WaitForExit()
    $global:LASTEXITCODE = $p.ExitCode
  }
  $stdout.Result
  $stderr.Result
}

$abis = @(@{ Abi = 'x86_64'; Tag = 'x86_64' }, @{ Abi = 'arm64-v8a'; Tag = 'arm64' })

# --- 1. dalvikvm: the test library's checks and the symbol lists -------------------
# gesher.Native2Test System.load()s libnative2.so (x86_64, or arm64-v8a through
# the translator); its checks: the shim's additions (semaphores, wide
# characters, libm and fenv, getopt_long, strptime, pwd, system properties,
# barriers, sysinfo, prctl, getrandom, zlib streams and gz files), dlopen of
# the platform's libraries by name and /system/lib64 path with dlsym on the
# handles and RTLD_DEFAULT and calls through them (EGL, OpenSL ES, AAudio,
# zlib), an app library by its bare name, flock, the execute bit, symbolic
# links and inotify's IN_MODIFY at the write. Then every symbol the symbol
# lists name, through dlsym(RTLD_DEFAULT).
foreach ($a in $abis) {
  $work = Join-Path $env:RUNNER_TEMP "native2-dvm-$($a.Tag)"
  New-Item -ItemType Directory -Force $work | Out-Null
  $out = N2Run "$n2Dir/dalvikvm.exe" @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
    '-Xint', '-Xusejit:false', '-cp', (NoDrive "$n2/native2-test.dex"), 'gesher.Native2Test',
    (NoDrive "$n2/$($a.Abi)/libnative2.so"), (NoDrive $work), (NoDrive "$n2/symbols-$($a.Abi).txt")) | Out-String
  $code = $LASTEXITCODE
  $arm64FpcrEvidence.Add(@{ Abi = $a.Tag; Mode = 'dalvikvm'; Exit = $code; Report = $out })
  $lines = $out -split "`r?`n"
  foreach ($part in 'extras', 'unicode', 'dlopen', 'files') {
    $line = $lines | Where-Object { $_ -like "native2 $part*" } | Select-Object -First 1
    if ($line -and $line -like "native2 $($part): ok*") {
      Write-Host "PASS  native2 ($($a.Tag)): $part through dalvikvm ($($line.Substring($line.IndexOf(':') + 2)))"
    } else {
      Write-Host "FAIL  native2 ($($a.Tag)): $part through dalvikvm (exit $code): $line"
      $failed++
    }
  }
  if (-not ($lines -contains 'native2: all passed')) { Write-Host $out }
  # The symbols: what real programs and apps import must all resolve (weak
  # imports may stay missing); the NDK's public API is a coverage figure.
  $gaps = @()
  foreach ($line in $lines | Where-Object { $_ -like 'native2 symbols *' }) {
    if ($line -match '^native2 symbols (ndk:\S+|libc\+\+_shared): (\d+) checked, (\d+) missing') {
      Write-Host "INFO  native2 ($($a.Tag)) $line"
    } elseif ($line -match '^native2 symbols (\S+): (\d+) checked, (\d+) missing') {
      Write-Host "INFO  native2 ($($a.Tag)) $line"
      if ([int]$Matches[3] -gt 0) { $gaps += $Matches[1] }
    }
  }
  if (-not $gaps -and ($lines | Where-Object { $_ -like 'native2 symbols *' })) {
    Write-Host "PASS  native2 ($($a.Tag)): e2e and third-party imports resolve in the shim (executable __libc_init is checked by gesher-exec program runs)"
  } else {
    Write-Host "FAIL  native2 ($($a.Tag)): imports the shim lacks in: $($gaps -join ', ')"
    $failed++
  }
}

# --- 2. Popular NDK libraries' own tests, through gesher-exec -----------------------
# Android ELF executables built by the NDK (tests/native2/build-third-party.sh)
# with the shim as their libc: zlib's example and minigzip against libz.so,
# libpng's pngtest, libjpeg-turbo's tjunittest, the sqlite3 shell on a WAL
# database, the openssl command, and a libc++ test.
foreach ($a in $abis) {
  $bin = Join-Path $n2 $a.Abi
  $tag = $a.Tag
  $work = Join-Path $env:RUNNER_TEMP "native2-exec-$tag"
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $work | Out-Null
  $exec = "$n2Dir/gesher-exec.exe"
  $env2 = @{ LD_LIBRARY_PATH = (NoDrive $bin); TMPDIR = (NoDrive $work) }
  function Elf([string] $Name, [string[]] $Arguments, [int] $Seconds = 900) {
    N2Run $exec (@('--elf', "$bin/$Name", $Name) + $Arguments) $work $env2 $Seconds
  }

  Check "native2 ($tag): zlib 1.3.1's example (deflate/inflate, gz files, dictionaries) against the shim's libz.so" {
    Elf 'zlib_example' @()
  } @('zlib version 1.', 'uncompress(): hello, hello!', 'gzread(): hello, hello!', 'gzgets() after gzseek: ',
      'inflate(): hello, hello!', 'large_inflate(): OK', 'after inflateSync(): hello, hello!',
      'inflate with dictionary: hello, hello!')

  Check "native2 ($tag): zlib's minigzip compresses and restores a file" {
    $text = (1..2000 | ForEach-Object { "line $_ of the minigzip test" }) -join "`n"
    [IO.File]::WriteAllText("$work/mg.txt", $text)
    $o = Elf 'zlib_minigzip' @('mg.txt') | Out-String
    $c1 = $LASTEXITCODE
    $gz = Test-Path "$work/mg.txt.gz"
    $o += Elf 'zlib_minigzip' @('-d', 'mg.txt.gz') | Out-String
    $same = (Test-Path "$work/mg.txt") -and ([IO.File]::ReadAllText("$work/mg.txt") -eq $text)
    if ($c1 -ne 0 -or $LASTEXITCODE -ne 0 -or -not $gz -or -not $same) { $global:LASTEXITCODE = 1 }
    "$o`ngz=$gz restored=$same"
  } @('gz=True restored=True')

  Check "native2 ($tag): libpng 1.6.43's pngtest" {
    Copy-Item "$bin/pngtest.png" $work
    Elf 'pngtest' @('pngtest.png', 'pngout.png')
  } @('libpng passes test')

  Check "native2 ($tag): libjpeg-turbo 3.0.4's tjunittest (TurboJPEG compress/decompress round trips, SIMD)" {
    $o = Elf 'tjunittest' @() 1500 | Out-String
    if ($o -match 'FAILED|ERROR') { $global:LASTEXITCODE = 1 }
    $o
  } @()

  Check "native2 ($tag): SQLite 3.46.1's shell: a WAL database, 20,000 rows, an index, a transaction, FTS5, the integrity check" {
    Copy-Item "$n2/sqlite_test.sql" $work
    Elf 'sqlite3' @('-batch', 'test.db', '.read sqlite_test.sql')
  } @('wal', '20000|100005000.0', '12345', '150010000.0', '1.4142', 'ok', 'sqlite_test: done')

  Check "native2 ($tag): OpenSSL 3.3.2's openssl: SHA-256, AES-256-CBC with PBKDF2 both ways, an EC P-256 key, a signature verified" {
    $data = [byte[]]::new(300000)
    [Random]::new(7).NextBytes($data)
    [IO.File]::WriteAllBytes("$work/data.bin", $data)
    $hash = (Get-FileHash "$work/data.bin" -Algorithm SHA256).Hash.ToLower()
    $o = ''
    $bad = @()
    foreach ($cmd in @(
        @('version'),
        @('dgst', '-sha256', 'data.bin'),
        @('enc', '-aes-256-cbc', '-pbkdf2', '-pass', 'pass:gesher', '-in', 'data.bin', '-out', 'data.enc'),
        @('enc', '-d', '-aes-256-cbc', '-pbkdf2', '-pass', 'pass:gesher', '-in', 'data.enc', '-out', 'data.dec'),
        @('genpkey', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256', '-out', 'key.pem'),
        @('pkey', '-in', 'key.pem', '-pubout', '-out', 'pub.pem'),
        @('dgst', '-sha256', '-sign', 'key.pem', '-out', 'data.sig', 'data.bin'),
        @('dgst', '-sha256', '-verify', 'pub.pem', '-signature', 'data.sig', 'data.bin'),
        @('rand', '-hex', '16'))) {
      $o += Elf 'openssl' $cmd | Out-String
      if ($LASTEXITCODE -ne 0) { $bad += $cmd[0] }
    }
    $dec = (Test-Path "$work/data.dec") -and ((Get-FileHash "$work/data.dec" -Algorithm SHA256).Hash.ToLower() -eq $hash)
    $digest = $o.Contains("= $hash")
    if ($bad -or -not $dec -or -not $digest) { $global:LASTEXITCODE = 1 } else { $global:LASTEXITCODE = 0 }
    "$o`nsha256 matches=$digest, decrypted=$dec, failed: $($bad -join ',')"
  } @('OpenSSL 3.3.2', 'Verified OK', 'sha256 matches=True', 'decrypted=True')

  Check "native2 ($tag): libc++ (iostreams, locale, regex, threads, futures, shared_mutex, exceptions, chrono, std::filesystem)" {
    Elf 'libcxx2_test' @((NoDrive $work))
  } @('libcxx2_test: all passed')
}

# --- 3. Through gesher-app: the test APKs, single process and on the device --------
# native2app.apk (lib/x86_64) and native2app-arm64.apk (lib/arm64-v8a only):
# the Activity's System.loadLibrary from the app's native library directory,
# then the library's checks, as an installed app sees them.
$bcpN2 = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dvmN2 = @("-Xbootclasspath:$bcpN2", "-Ximage:$(NoDrive "$root/system")/framework/boot.art", '-Xint', '-Xusejit:false')
$apks = @(@{ Apk = 'native2app.apk'; Tag = 'x86_64' }, @{ Apk = 'native2app-arm64.apk'; Tag = 'arm64' })
$n2Lines = @('Native2Activity: native2 extras: ok', 'Native2Activity: native2 dlopen: ok', 'Native2Activity: native2 files: ok',
  'Native2Activity: native2 unicode: ok; pinned ICU4X boundaries; plain/_l/wctype/wctrans and errors',
  'Native2Activity: native2 bitmap: ok; direct pixels and PNG callback round trip',
  'Native2Activity: native3 Java Bitmap sees native pixels: true',
  'Native2Activity: native2 fonts: ok',
  'Native2Activity: native2: all passed', 'Native2Activity onResume: all passed', 'gesher-app: done')
foreach ($a in $apks) {
  Check "native2 ($($a.Tag)): gesher-app runs $($a.Apk) (single process): System.loadLibrary from the APK, the library's checks pass" {
    $fpcrOut = N2Run "$n2Dir/dalvikvm.exe" ($dvmN2 + @('com.gesher.app.Main', (NoDrive "$n2/$($a.Apk)"))) (Get-Location).Path @{} 900 | Out-String
    $arm64FpcrEvidence.Add(@{ Abi = $a.Tag; Mode = 'single'; Exit = $LASTEXITCODE; Report = $fpcrOut })
    $fpcrOut
  } $n2Lines
}

$savedData = $env:ANDROID_DATA
$savedLogs = $env:GESHER_LOGS
$env:ANDROID_DATA = NoDrive "$root/data-native2"
$env:GESHER_LOGS = NoDrive "$root/logs-native2"
New-Item -ItemType Directory -Force "$root/data-native2" | Out-Null
$env:GESHER_SHORTCUTS = '0'
try {
  foreach ($a in $apks) {
    Check "native2 ($($a.Tag)): the Gesher device: $($a.Apk)'s process (started by the system server) loads its library and passes the checks" {
      $fpcrOut = N2Run "$n2Dir/dalvikvm.exe" ($dvmN2 + @('com.gesher.app.Launcher', 'launch', (NoDrive "$n2/$($a.Apk)"), '--headless')) `
        (Get-Location).Path @{} 900 | Out-String
      $arm64FpcrEvidence.Add(@{ Abi = $a.Tag; Mode = 'device'; Exit = $LASTEXITCODE; Report = $fpcrOut })
      $fpcrOut
    } ($n2Lines + @('gesher-app: app process for com.gesher.native2'))
  }
} finally {
  $sd = N2Run "$n2Dir/dalvikvm.exe" ($dvmN2 + @('com.gesher.app.Launcher', 'shutdown')) (Get-Location).Path @{} 120 | Out-String
  if ($LASTEXITCODE -ne 0) { Write-Host "INFO  native2: the device shutdown: exit $LASTEXITCODE $sd" }
  $env:ANDROID_DATA = $savedData
  if ($savedLogs) { $env:GESHER_LOGS = $savedLogs } else { Remove-Item Env:GESHER_LOGS -ErrorAction SilentlyContinue }
  Remove-Item Env:GESHER_SHORTCUTS -ErrorAction SilentlyContinue
}
