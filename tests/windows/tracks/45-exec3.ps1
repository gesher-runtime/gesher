# The real launcher's shell, not a shell mock. Each case has an exact output
# and status assertion; pipelines exceed a pipe buffer, subshells preserve
# state without inheriting mutations, and serialized state exceeds 32 KiB.
Check 'exec3: binder startup waits for a protocol connection with bounded failure' {
  & "$Dir/exec3_binder_ready_probe.exe"
} @('exec3 binder readiness: 10 checks, 0 failed')

Check 'exec3: local-exec FS rewrite preserves registers, flags, SIMD and thread isolation' {
  & "$Dir/exec3_tls_probe.exe"
} @('exec3 TLS probe: 0 failed')

Check 'exec3: executable ELF TLS at API29 (constructor, initialized data, tbss, alignment, integer/SSE, pthread)' {
  & "$Dir/gesher-exec.exe" --elf "$Dir/libexec3tls.so" 'libexec3tls.so'
} @('exec3 TLS executable: PASS constructor, initialized data, tbss, alignment, integer/SSE local-exec and pthread isolation')

$exec3ShellLog = & python "$PSScriptRoot/../../exec/shell-smoke.py" "$Dir/gesher-exec.exe" 2>&1
$exec3ShellCode = $LASTEXITCODE
$exec3ShellLog | ForEach-Object { Write-Host $_ }
if ($exec3ShellCode -ne 0) {
  Write-Host "FAIL  exec3: shell regression suite (exit $exec3ShellCode)"
  $failed++
} else {
  Write-Host 'PASS  exec3: shell regression suite through the real launcher'
}

Check 'exec3: ProcessBuilder shell with an open stdin (incremental output, read and multiline commands)' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/exec-test.dex") gesher.Exec3Test
} @('Exec3Test PASS incremental stdin, read, multiline loop, pipeline, substitution, EXIT trap and status')

Check 'exec3: Java process signals and binary tools (destroy, destroyForcibly, ps, kill, cat, sleep)' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/exec-test.dex") gesher.Exec3ProcessTest (NoDrive $Dir)
} @('Exec3ProcessTest PASS ProcessBuilder runs an API29 executable with local-exec TLS',
    'Exec3ProcessTest PASS destroy runs the native SIGTERM handler and ps lists its pid',
    'Exec3ProcessTest PASS destroyForcibly kills with status 137',
    'Exec3ProcessTest PASS kill sends SIGTERM through the native signal mailbox',
    'Exec3ProcessTest PASS cat preserves binary stdin and stdout',
    'Exec3ProcessTest PASS sleep delays for the requested duration',
    'Exec3ProcessTest PASS mailbox traps run while stdin is idle and interrupt the read builtin')

Check 'exec3: shared properties through Java, native ELF and tools (read-only, serials, persistence)' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpApp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/exec-test.dex") gesher.Exec3PropertiesTest (NoDrive $Dir) --load-runtime
} @('Exec3PropertiesTest PASS Java, getprop/setprop and ELF bionic share live properties',
    'Exec3PropertiesTest PASS cached ELF property callbacks and serials observe another process',
    'Exec3PropertiesTest PASS ro properties are write-once and invalid names fail',
    'Exec3PropertiesTest PASS ELF serial ABI, stable ro callbacks, long legacy values, foreach and cross-process wait/wait_any',
    'Exec3PropertiesTest PASS framework reportSyspropChanged invokes registered Java callbacks',
    'Exec3PropertiesTest PASS a new device boot clears volatile properties and loads persist properties')

$exec3SavedLogFile = $env:GESHER_LOG_FILE
$env:GESHER_LOG_FILE = "$root/exec3-standalone.logcat"
try {
  Check 'exec3: Java/NDK logcat records, filters, formats, clear and follow' {
    & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpApp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
      -Xint '-Xusejit:false' -cp (NoDrive "$Dir/exec-test.dex") gesher.Exec3LogTest (NoDrive $Dir) --load-runtime
  } @('Exec3LogTest PASS actual Java/NDK records, tags, priority filters, nine formats and live loggability properties',
      'Exec3LogTest PASS clear removes old records and a live follower observes new records after clear')
} finally {
  if ($null -eq $exec3SavedLogFile) { Remove-Item Env:GESHER_LOG_FILE -ErrorAction SilentlyContinue }
  else { $env:GESHER_LOG_FILE = $exec3SavedLogFile }
}

# Run those same native and Java operations from an installed APK attached to
# the real system server. This does not substitute a host-only dalvikvm for an
# app process: the APK's Activity launches binaries extracted in nativeLibraryDir.
function InvokeExec3Timed([string[]] $Arguments, [int] $Seconds = 240) {
  $info = [Diagnostics.ProcessStartInfo]::new("$Dir/dalvikvm.exe")
  foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $process = [Diagnostics.Process]::Start($info)
  $stdout = $process.StandardOutput.ReadToEndAsync()
  $stderr = $process.StandardError.ReadToEndAsync()
  $timedOut = -not $process.WaitForExit($Seconds * 1000)
  if ($timedOut) { $process.Kill($true); $process.WaitForExit() }
  $text = ''
  if ($stdout.Wait(10000)) { $text += $stdout.Result } else { $timedOut = $true; $text += '(stdout did not close)' }
  if ($stderr.Wait(10000)) { $text += $stderr.Result } else { $timedOut = $true; $text += '(stderr did not close)' }
  if ($timedOut) { $text += "`n(exec3 app timed out after $Seconds seconds)" }
  return @{ Code = $(if ($timedOut) { -1 } else { $process.ExitCode }); Text = $text }
}

if (-not (Test-Path "$Dir/exec3.apk")) {
  Write-Host 'FAIL  exec3: the on-device process APK is missing'
  $failed++
} else {
  Copy-Item "$Dir/build.prop" (Join-Path $root 'system') -Force
  Check 'exec3: native processes and Java suites from a single-process APK' {
    & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpApp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
      -Xint '-Xusejit:false' com.gesher.app.Main (NoDrive "$Dir/exec3.apk")
  } @('exec2: SUMMARY pass=9 fail=0',
      'Exec3DeviceTest PASS native fork/exec, spawn, popen, system, signals and waitpid in an app',
      'Exec3DeviceTest PASS shell, Java process signals, tools and shared properties on the device',
      'Exec3LogTest PASS clear removes old records and a live follower observes new records after clear')
  $exec3SavedEnvironment = @{}
  foreach ($exec3Name in 'ANDROID_DATA', 'GESHER_BINDER', 'GESHER_LOGS', 'GESHER_SHORTCUTS') {
    $exec3SavedEnvironment[$exec3Name] = [Environment]::GetEnvironmentVariable($exec3Name)
  }
  $exec3Logs = Join-Path $root 'exec3-device-logs'
  New-Item -ItemType Directory -Force $exec3Logs | Out-Null
  $env:ANDROID_DATA = NoDrive "$root/data-exec3"
  $env:GESHER_LOGS = NoDrive $exec3Logs
  $env:GESHER_SHORTCUTS = '0'
  Remove-Item Env:GESHER_BINDER -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force "$root/data-exec3" | Out-Null
  $exec3DeviceVm = @("-Xbootclasspath:$bcpApp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
    '-Xint', '-Xusejit:false')
  try {
    # Launcher installs and extracts the APK before starting its real app process.
    $exec3Target = InvokeExec3Timed ($exec3DeviceVm + 'com.gesher.app.Launcher', 'install', (NoDrive "$Dir/exec3target.apk"))
    if ($exec3Target.Code -ne 0) { throw "exec3 target install failed: $($exec3Target.Text)" }
    $exec3App = InvokeExec3Timed ($exec3DeviceVm + 'com.gesher.app.Launcher', 'launch', (NoDrive "$Dir/exec3.apk"), '--headless', '--ez', 'exec3.commands', 'true')
    $exec3Wanted = @(
      'exec2: SUMMARY pass=9 fail=0',
      'gesher-app: app process for com.gesher.exec3',
      'Exec3DeviceTest PASS native fork/exec, spawn, popen, system, signals and waitpid in an app',
      'Exec3LogTest PASS clear removes old records and a live follower observes new records after clear',
      'Exec3CommandsTest PASS pm list packages and path query the actual device package manager',
      'Exec3CommandsTest PASS am start resumes a separate app and broadcast reaches its real receiver',
      'Exec3CommandsTest PASS am force-stop terminates the target app process',
      'Exec3DeviceTest PASS shell, Java process signals, tools and shared properties on the device')
    $exec3Missing = $exec3Wanted | Where-Object { -not $exec3App.Text.Contains($_) }
    if ($exec3App.Code -eq 0 -and -not $exec3Missing -and -not $exec3App.Text.Contains('Exec3DeviceTest FAIL')) {
      Write-Host 'PASS  exec3: native processes, shell, signals, tools and shared properties from an APK on the device'
    } else {
      Write-Host "FAIL  exec3: on-device process suite (exit $($exec3App.Code); missing: $($exec3Missing -join ', '))"
      Write-Host $exec3App.Text
      Get-ChildItem -LiteralPath $exec3Logs -Filter '*.log' | ForEach-Object {
        Get-Content -LiteralPath $_.FullName -Tail 100 -ErrorAction SilentlyContinue | Out-Host
      }
      $failed++
    }
  } catch {
    Write-Host "FAIL  exec3: installed-device test could not complete: $_"
    $failed++
  } finally {
    $exec3Shutdown = InvokeExec3Timed ($exec3DeviceVm + 'com.gesher.app.Launcher', 'shutdown') 60
    if ($exec3Shutdown.Code -ne 0) { Write-Host "INFO  exec3: device shutdown: $($exec3Shutdown.Text)" }
    foreach ($exec3Name in $exec3SavedEnvironment.Keys) {
      if ($null -eq $exec3SavedEnvironment[$exec3Name]) { Remove-Item "Env:$exec3Name" -ErrorAction SilentlyContinue }
      else { Set-Item "Env:$exec3Name" $exec3SavedEnvironment[$exec3Name] }
    }
  }
}
