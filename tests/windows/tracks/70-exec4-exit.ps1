# The server's actual generation-aware result component, loaded from gesher-app.jar.
$exitBcp = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') |
  ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
CheckTimed 'exec4 exit: only the latest bound PID/start can publish launcher success' "$Dir/dalvikvm.exe" @(
  "-Xbootclasspath:$exitBcp", "-Ximage:$(NoDrive "$root/system/framework/boot.art")",
  '-cp', (NoDrive "$Dir/graphics-tests.dex"), 'gesher.Exec4ExitStatusTest') @(
  'a new start clears the previous package success',
  'a late previous-process exit cannot fill the new start',
  'PID reuse cannot reuse the previous start sequence', 'Exec4ExitStatusTest all passed')

# Exercise the actual persistent server, three distinct processes of one
# package and the launcher's native process exit status, not just log text.
$exitLauncher = @("-Xbootclasspath:$exitBcp", "-Ximage:$(NoDrive "$root/system/framework/boot.art")",
  'com.gesher.app.Launcher')
$exitPackage = 'com.gesher.exitstatus'
$exitEnvironment = @{ GESHER_APP_WINDOW = 'hidden' }
try {
  $null = Aot2Run "$Dir/dalvikvm.exe" ($exitLauncher + 'shutdown') @{} 120
  $exitInstall = Aot2Run "$Dir/dalvikvm.exe" ($exitLauncher + 'install', (NoDrive "$Dir/exitstatusapp.apk")) $exitEnvironment 180
  if ($exitInstall.Exit -ne 0) { throw "exit fixture install failed: $($exitInstall.Out)" }
  foreach ($exitCase in @(
      @{ Activity = '.NormalActivity'; Code = 0; Marker = 'normal activity started' },
      @{ Activity = '.AbnormalActivity'; Code = 23; Marker = 'exiting with code 23' },
      @{ Activity = '.NormalActivity'; Code = 0; Marker = 'normal activity started' })) {
    $exitRun = Aot2Run "$Dir/dalvikvm.exe" ($exitLauncher + 'launch', $exitPackage,
      '--activity', $exitCase.Activity, '--hidden-window', '--keep-task') $exitEnvironment 120
    $exitExpected = "gesher-app: the process of $exitPackage ended (exit code $($exitCase.Code))"
    $exitMatches = if ($exitCase.Code -eq 0) { $exitRun.Exit -eq 0 } else { $exitRun.Exit -ne 0 }
    if ($exitMatches -and $exitRun.Out.Contains($exitExpected) -and
        $exitRun.Out.Contains("Exec4 exit app: $($exitCase.Marker)")) {
      Write-Host "PASS  exec4 exit: $($exitCase.Activity) returns fresh app code $($exitCase.Code) and launcher code $($exitRun.Exit)"
      Write-Host "INFO  exec4 exit: $exitExpected"
    } else {
      Write-Host "FAIL  exec4 exit: $($exitCase.Activity), app code $($exitCase.Code), launcher code $($exitRun.Exit)"
      Write-Host $exitRun.Out
      $script:failed++
    }
  }
} catch {
  Write-Host "FAIL  exec4 exit: real launcher sequence: $_"
  $script:failed++
} finally {
  $null = Aot2Run "$Dir/dalvikvm.exe" ($exitLauncher + 'uninstall', $exitPackage) $exitEnvironment 120
  $null = Aot2Run "$Dir/dalvikvm.exe" ($exitLauncher + 'shutdown') @{} 120
}
