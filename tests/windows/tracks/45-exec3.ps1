# The real launcher's shell, not a shell mock. Each case has an exact output
# and status assertion; pipelines exceed a pipe buffer, subshells preserve
# state without inheriting mutations, and serialized state exceeds 32 KiB.
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
} @('Exec3ProcessTest PASS destroy runs the native SIGTERM handler and ps lists its pid',
    'Exec3ProcessTest PASS destroyForcibly kills with status 137',
    'Exec3ProcessTest PASS kill sends SIGTERM through the native signal mailbox',
    'Exec3ProcessTest PASS cat preserves binary stdin and stdout',
    'Exec3ProcessTest PASS sleep delays for the requested duration',
    'Exec3ProcessTest PASS mailbox traps run while stdin is idle and interrupt the read builtin')

Check 'exec3: shared properties through Java, native ELF and tools (read-only, serials, persistence)' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/exec-test.dex") gesher.Exec3PropertiesTest (NoDrive $Dir)
} @('Exec3PropertiesTest PASS Java, getprop/setprop and ELF bionic share live properties',
    'Exec3PropertiesTest PASS cached ELF property callbacks and serials observe another process',
    'Exec3PropertiesTest PASS ro properties are write-once and invalid names fail',
    'Exec3PropertiesTest PASS a new device boot clears volatile properties and loads persist properties')

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
  $null = $stdout.Wait(10000), $stderr.Wait(10000)
  $text = $stdout.Result + $stderr.Result
  if ($timedOut) { $text += "`n(exec3 app timed out after $Seconds seconds)" }
  return @{ Code = $(if ($timedOut) { -1 } else { $process.ExitCode }); Text = $text }
}

if (-not (Test-Path "$Dir/exec3.apk")) {
  Write-Host 'FAIL  exec3: the on-device process APK is missing'
  $failed++
} else {
  Copy-Item "$Dir/build.prop" (Join-Path $root 'system') -Force
  $exec3PreviousBinder = $env:GESHER_BINDER
  $env:GESHER_BINDER = "exec3-app-$PID-$(Get-Random)"
  $exec3Logs = Join-Path $root 'exec3-device-logs'
  New-Item -ItemType Directory -Force $exec3Logs | Out-Null
  $exec3Binderd = $null
  $exec3Manager = $null
  $exec3Server = $null
  try {
    $exec3Binderd = Start-Process "$Dir/binderd.exe" -ArgumentList $env:GESHER_BINDER -PassThru -WindowStyle Hidden `
      -RedirectStandardOutput "$exec3Logs/binderd.out" -RedirectStandardError "$exec3Logs/binderd.err"
    $exec3Manager = Start-Process "$Dir/servicemanager.exe" -PassThru -WindowStyle Hidden `
      -RedirectStandardOutput "$exec3Logs/servicemanager.out" -RedirectStandardError "$exec3Logs/servicemanager.err"
    $exec3DeviceVm = @("-Xbootclasspath:$bcpApp", "-Ximage:$(NoDrive "$root/system")/framework/boot.art",
      '-Xint', '-Xusejit:false')
    $exec3Server = Start-Process "$Dir/dalvikvm.exe" -PassThru -WindowStyle Hidden `
      -ArgumentList ($exec3DeviceVm + 'com.gesher.server.SystemServer', (NoDrive "$Dir/exec3.apk")) `
      -RedirectStandardOutput "$exec3Logs/system_server.out" -RedirectStandardError "$exec3Logs/system_server.err"
    $exec3App = InvokeExec3Timed ($exec3DeviceVm + 'com.gesher.app.Main', '--system-server', (NoDrive "$Dir/exec3.apk"))
    $exec3Wanted = @(
      'exec2: SUMMARY pass=9 fail=0',
      'Exec3DeviceTest PASS native fork/exec, spawn, popen, system, signals and waitpid in an app',
      'Exec3DeviceTest PASS shell, Java process signals, tools and shared properties on the device')
    $exec3Missing = $exec3Wanted | Where-Object { -not $exec3App.Text.Contains($_) }
    if ($exec3App.Code -eq 0 -and -not $exec3Missing -and -not $exec3App.Text.Contains('Exec3DeviceTest FAIL')) {
      Write-Host 'PASS  exec3: native processes, shell, signals, tools and shared properties from an APK on the device'
    } else {
      Write-Host "FAIL  exec3: on-device process suite (exit $($exec3App.Code); missing: $($exec3Missing -join ', '))"
      Write-Host $exec3App.Text
      Get-Content "$exec3Logs/system_server.out", "$exec3Logs/system_server.err" -ErrorAction SilentlyContinue |
        Select-Object -Last 100 | Out-Host
      $failed++
    }
  } finally {
    foreach ($exec3Process in $exec3Server, $exec3Manager, $exec3Binderd) {
      if ($exec3Process -and -not $exec3Process.HasExited) { $exec3Process.Kill($true) }
    }
    if ($null -eq $exec3PreviousBinder) { Remove-Item Env:GESHER_BINDER -ErrorAction SilentlyContinue }
    else { $env:GESHER_BINDER = $exec3PreviousBinder }
  }
}
