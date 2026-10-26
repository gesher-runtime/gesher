# Learn the actual Gesher startup path through ART's pinned boot-profile saver.
# This is diagnostic input for the unchanged profile-vs-speed performance gate.
try {
  $learnRoot = Join-Path $aotRoot 'boot-startup-profile'
  $learnData = Join-Path $learnRoot 'data'
  $learnCur = Join-Path $learnData 'misc/profiles/cur/0/com.gesher.lifecycle/primary.prof'
  $learnRef = Join-Path $learnData 'misc/profiles/ref/com.gesher.lifecycle/primary.prof'
  foreach ($path in $learnCur, $learnRef) {
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    [IO.File]::WriteAllBytes($path, [byte[]]::new(0))
  }
  $learnEnv = @{ ANDROID_DATA = (NoDrive $learnData); GESHER_APP_EXECUTION = 'jit'; GESHER_LOG_EXECUTION = '1' }
  $learnArgs = @("-Xbootclasspath:$aotBcpApp", $aotImage, '-Xusejit:true', '-Xjitsaveprofilinginfo',
    '-Xps-profile-boot-class-path', '-Xps-profile-aot-code', '-Xps-min-first-save-ms:0',
    '-Xps-min-methods-to-save:1', '-Xps-min-classes-to-save:1',
    'com.gesher.app.Main', (NoDrive "$Dir/lifecycle.apk"))
  $learn = Aot2Run "$Dir/dalvikvm.exe" $learnArgs $learnEnv 300
  $learnSize = Aot2Size $learnCur
  Write-Host "INFO  aot3: boot collector exit $($learn.Exit), profile $learnSize bytes, saved-at-exit marker $($learn.Out.Contains('gesher: JIT profile saved at exit'))"
  if ($learn.Exit -ne 0 -or -not $learn.Out.Contains('gesher-app: done') -or
      -not $learn.Out.Contains('gesher: JIT profile saved at exit') -or $learnSize -eq 0) {
    throw "ART boot startup profile was not saved (exit $($learn.Exit)): $($learn.Out)"
  }
  $dumpArgs = @('--dump-classes-and-methods', "--profile-file=$(NoDrive $learnCur)")
  foreach ($jar in 'core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') {
    $dumpArgs += @("--apk=$(NoDrive "$Dir/$jar.jar")", "--dex-location=$jar.jar")
  }
  $dumpArgs += @("--apk=$(NoDrive "$Dir/lifecycle.apk")", "--dex-location=$(NoDrive "$Dir/lifecycle.apk")")
  $dump = Aot2Run "$Dir/profman.exe" $dumpArgs @{} 120
  $rows = @($dump.Out -split "`n" | ForEach-Object { $_.Trim() } |
    Where-Object { $_ -match '^[HSP]*(L|\[)' } | Sort-Object -Unique)
  $frameworkMethods = @($rows | Where-Object { $_ -match '^[HSP]*Landroid/.*->' })
  Aot3Check 'ART boot profile saver and profman record actual framework startup methods' ($dump.Exit -eq 0 -and $frameworkMethods.Count -gt 0) ($learn.Out + $dump.Out)
  if ($dump.Exit -eq 0 -and $frameworkMethods.Count -gt 0) {
    Write-Host "INFO  aot3: boot startup profile: $(Aot2Size $learnCur) bytes, $($rows.Count) rows, $($frameworkMethods.Count) framework methods; existing timing criteria unchanged"
    Write-Host 'BOOT_STARTUP_PROFILE_BEGIN'
    $rows | ForEach-Object { Write-Host $_ }
    Write-Host 'BOOT_STARTUP_PROFILE_END'
  }
} catch {
  Aot3Check "boot-profile learning threw: $($_.Exception.Message)" $false $_.ScriptStackTrace
}
