# Dedicated callers preserve Native2's existing no-INTERNET manifest/modules.
$resolverOracle = Join-Path $env:RUNNER_TEMP "net4-resolver82-$([guid]::NewGuid()).txt"
$resolverData = Join-Path $root 'data-net4-resolver82'
$resolverLogs = Join-Path $root 'logs-net4-resolver82'
$resolverEnvironment = @{ ANDROID_DATA=(NoDrive $resolverData); GESHER_LOGS=(NoDrive $resolverLogs);
  GESHER_SHORTCUTS='0'; GESHER_APP_WINDOW='headless' }
$resolverLauncher = $dvmN2 + 'com.gesher.app.Launcher'
function Net4ResolverCheck([string] $Name, $Run, [bool] $Okay) {
  Write-Host $Run.Out
  if ($Run.Exit -eq 0 -and $Okay -and $Run.Out -notmatch '(?m)^(?:Net4ResolverActivity: )?FAIL net4 resolver:') {
    Write-Host "PASS  net4 resolver: $Name"
  } else {
    Write-Host "FAIL  net4 resolver: $Name (exit $($Run.Exit))"
    $script:failed++
  }
}
try {
  New-Item -ItemType Directory -Force $resolverData, $resolverLogs | Out-Null
  $oracleScript = Join-Path $PSScriptRoot '../net/LegacyResolverOracle.ps1'
  $oracleRun = Aot2Run (Get-Command pwsh).Source @('-NoProfile', '-File', $oracleScript, '-Destination', $resolverOracle) @{} 60
  Net4ResolverCheck 'independent WinSDK process decodes actual Win64 host/service data' $oracleRun `
    ($oracleRun.Out.Contains('WinSDK legacy resolver oracle: eight real host/service rows') -and (Test-Path -LiteralPath $resolverOracle))
  if ($oracleRun.Exit -ne 0 -or -not (Test-Path -LiteralPath $resolverOracle)) { throw 'independent Windows oracle failed' }
  foreach ($abi in 'x86_64', 'arm64-v8a') {
    foreach ($kind in 'allow', 'deny') {
      $allowed = if ($kind -eq 'allow') { 1 } else { 0 }
      $marker = "net4 resolver: all passed; allowed=$allowed;"
      $policy = @{ GESHER_INET="$allowed"; LD_LIBRARY_PATH=(NoDrive "$n2/$abi") }
      $arguments = @('--elf', "$n2/$abi/net4_resolver_test", 'net4_resolver_test', $kind)
      if ($allowed) { $arguments += (NoDrive $resolverOracle) }
      $elf = Aot2Run "$n2Dir/gesher-exec.exe" $arguments $policy 90
      Net4ResolverCheck "$abi standalone/${kind}: Linux layouts, actual data, errors, permission, mutation and four-thread ownership" $elf `
        ($elf.Out.Contains($marker) -and (-not $allowed -or $elf.Out.Contains('; oracle=8')))
      $vm = Aot2Run "$n2Dir/dalvikvm.exe" @("-Xbootclasspath:$bcp", "-Ximage:$(NoDrive "$root/system/framework/boot.art")",
        '-Xint', '-Xusejit:false', '-cp', (NoDrive "$n2/native2-test.dex"), 'gesher.Net4ResolverTest',
        (NoDrive "$n2/$abi/libnet4resolver.so"), $kind) $policy 90
      Net4ResolverCheck "$abi ART/${kind}: actual JNI calls and thread-local host/service/error state" $vm ($vm.Out.Contains($marker))
      $name = if ($allowed) { 'net4resolverapp' } else { 'net4resolverdenied' }
      $package = if ($allowed) { 'com.gesher.net4resolver' } else { 'com.gesher.net4resolver.denied' }
      $apk = NoDrive "$n2/$name-$abi.apk"
      foreach ($mode in 'single', 'device') {
        $null = Aot2Run "$n2Dir/dalvikvm.exe" ($resolverLauncher + 'shutdown') $resolverEnvironment 120
        if ($mode -eq 'single') {
          $app = Aot2Run "$n2Dir/dalvikvm.exe" ($dvmN2 + @('com.gesher.app.Main', $apk)) $resolverEnvironment 120
          $fresh = $app.Out.Contains('gesher-app: done')
        } else {
          $app = Aot2Run "$n2Dir/dalvikvm.exe" ($resolverLauncher + @('launch', $apk, '--headless')) $resolverEnvironment 120
          $fresh = $app.Out.Contains("gesher-app: the process of $package ended (exit code 0)")
        }
        Net4ResolverCheck "$abi $mode APK/${kind}: manifest INTERNET policy and actual native resolver checks" $app `
          ($fresh -and $app.Out.Contains($marker) -and $app.Out.Contains("Net4ResolverActivity onResume: all passed; allowed=$allowed"))
        if ($mode -eq 'device') {
          $remove = Aot2Run "$n2Dir/dalvikvm.exe" ($resolverLauncher + @('uninstall', $package)) $resolverEnvironment 120
          if ($remove.Exit -ne 0) { throw "resolver APK uninstall failed: $($remove.Out)" }
        }
      }
    }
  }
} catch {
  Write-Host "FAIL  net4 resolver: fixture exception: $_"
  $script:failed++
} finally {
  foreach ($package in 'com.gesher.net4resolver', 'com.gesher.net4resolver.denied') {
    $null = Aot2Run "$n2Dir/dalvikvm.exe" ($resolverLauncher + @('uninstall', $package)) $resolverEnvironment 120
  }
  $stopped = Aot2Run "$n2Dir/dalvikvm.exe" ($resolverLauncher + 'shutdown') $resolverEnvironment 120
  $left = @(Get-ChildItem -LiteralPath (Join-Path $resolverData 'app') -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like 'com.gesher.net4resolver-*' -or $_.Name -like 'com.gesher.net4resolver.denied-*' })
  if ($stopped.Exit -eq 0 -and $left.Count -eq 0) { Write-Host 'PASS  net4 resolver: dedicated device shuts down after APK/code cleanup' }
  else { Write-Host "FAIL  net4 resolver: device shutdown (exit $($stopped.Exit))"; $script:failed++ }
  Remove-Item -LiteralPath $resolverOracle -Force -ErrorAction SilentlyContinue
}
