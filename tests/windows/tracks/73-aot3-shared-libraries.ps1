# Real system Java jars, LoadedApk dependency loaders and matching ART artifacts.
function Aot3SharedCheck([string] $Name, [bool] $Okay, [string] $Output) {
  if ($Okay) { Write-Host "PASS  aot3 shared libraries: $Name" }
  else {
    Write-Host "FAIL  aot3 shared libraries: $Name"
    $script:failed++
    ($Output -split "`n" | Select-Object -Last 100) | Out-Host
  }
}

$sharedRoot = Join-Path $aotRoot 'shared-libraries'
$sharedPackage = 'com.gesher.sharedapp'
$sharedConfig = Join-Path $root 'system/etc/permissions/gesher-shared73.xml'
$sharedBase = Join-Path $root 'system/framework/gesher-shared73-base.jar'
$sharedEntry = Join-Path $root 'system/framework/gesher-shared73-entry.jar'
$sharedOwned = @($sharedConfig, $sharedBase, $sharedEntry)
$sharedEnvironment = @{ GESHER_LOG_EXECUTION = '1'; GESHER_APP_WINDOW = 'headless'; GESHER_APP_EXECUTION = 'jit' }
$sharedPkgLog = Join-Path $root "logs/$sharedPackage.log"
$sharedServerLog = Join-Path $root 'logs/system_server.log'
$sharedCode = $null
$sharedFilesCreated = $false
$sharedBuildProp = Join-Path $root 'system/build.prop'
$sharedPropertiesBytes = $null
try {
  if (@($sharedOwned | Where-Object { Test-Path -LiteralPath $_ }).Count) {
    throw 'A shared73 fixture path already exists; refusing to overwrite it'
  }
  New-Item -ItemType Directory -Force $sharedRoot, (Split-Path $sharedConfig) | Out-Null
  $unit = Aot2Run "$Dir/dalvikvm.exe" @("-Xbootclasspath:$aotBcpApp", $aotImage,
    '-Xint', '-Xusejit:false', '-cp', (NoDrive "$Dir/graphics-tests.dex"), 'gesher.Aot3SharedLibraryTest',
    (NoDrive "$Dir/build.prop"), (NoDrive "$Dir/framework-res.apk"), (NoDrive "$sharedRoot/registry"),
    (NoDrive "$Dir/shared-base-v1.jar"), (NoDrive "$Dir/shared-base-v2.jar"), (NoDrive "$Dir/shared-entry.jar")) @{} 120
  Write-Host $unit.Out
  Aot3SharedCheck 'actual ART validates required/optional libraries, SDK gates, graph ownership, cycles and same-path jar updates' `
    ($unit.Exit -eq 0 -and $unit.Out.Contains('Aot3SharedLibraryTest all passed: 15 real-jar/registry controls') -and $unit.Out -notmatch '(?m)^FAIL') $unit.Out
  if ($unit.Exit -ne 0) { throw 'Shared-library graph controls failed' }

  $null = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'shutdown') @{} 120
  # Freeze the background optimizer while exercising deliberately stale files.
  # 61/63 already verify its normal idle behavior; their gates are unchanged.
  # This existing device policy is restored byte-for-byte in finally.
  $sharedPropertiesBytes = [IO.File]::ReadAllBytes($sharedBuildProp)
  [IO.File]::AppendAllText($sharedBuildProp, "`ngesher.dexopt.idle-ms=3600000`n", [Text.UTF8Encoding]::new($false))
  $sharedFilesCreated = $true
  Copy-Item "$Dir/shared-base-v1.jar" $sharedBase
  Copy-Item "$Dir/shared-entry.jar" $sharedEntry
  $baseAttribute = [Security.SecurityElement]::Escape((NoDrive $sharedBase))
  $entryAttribute = [Security.SecurityElement]::Escape((NoDrive $sharedEntry))
  [IO.File]::WriteAllText($sharedConfig, "<permissions><library name=`"gesher.shared.base`" file=`"$baseAttribute`"/><library name=`"gesher.shared.entry`" file=`"$entryAttribute`" dependency=`"gesher.shared.base`"/></permissions>", [Text.UTF8Encoding]::new($false))
  $sharedApk = Join-Path $sharedRoot 'sharedlibraryapp.apk'
  Copy-Item "$Dir/sharedlibraryapp.apk" $sharedApk
  $profileText = Join-Path $sharedRoot 'profile.txt'
  $sharedProfile = Join-Path $sharedRoot 'primary.prof'
  [IO.File]::WriteAllText($profileText, "Lcom/gesher/sharedapp/SharedActivity;`nHSLcom/gesher/sharedapp/SharedActivity;->onCreate(Landroid/os/Bundle;)V`nHSLcom/gesher/sharedapp/SharedActivity;->hot(I)I`n", [Text.UTF8Encoding]::new($false))
  $created = Aot2Run "$Dir/profman.exe" @("--create-profile-from=$(NoDrive $profileText)",
    "--reference-profile-file=$(NoDrive $sharedProfile)", "--apk=$(NoDrive $sharedApk)", "--dex-location=$(NoDrive $sharedApk)")
  if ($created.Exit -ne 0 -or (Aot2Size $sharedProfile) -eq 0) { throw "Shared profile creation failed: $($created.Out)" }
  Aot3Metadata (Join-Path $sharedRoot 'sharedlibraryapp.dm') ([IO.File]::ReadAllBytes($sharedProfile))
  $null = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'shutdown') @{} 120
  $null = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'uninstall', $sharedPackage) $sharedEnvironment 180
  $installed = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'install', (NoDrive $sharedApk)) $sharedEnvironment 400
  if ($installed.Exit -ne 0 -or $installed.Out -notmatch 'installed com\.gesher\.sharedapp \(version 1\) in (\S+)') {
    throw "Shared app install failed: $($installed.Out)"
  }
  $sharedCode = $Matches[1]
  $policy = [IO.File]::ReadAllText($sharedServerLog)
  if (-not $policy.Contains('background dexopt runs once the device is idle for 3600000 ms')) {
    throw 'The real server did not apply the stale-artifact control policy'
  }
  Write-Host 'INFO  aot3 shared libraries: the real background optimizer is deferred for this stale-artifact control; normal idle gates ran unchanged in61/63'

  function Aot3SharedArtifacts([string] $Phase) {
    $odex = "$sharedCode/oat/x86_64/base.odex"
    $art = "$sharedCode/oat/x86_64/base.art"
    $header = Aot2Run "$Dir/oatdump.exe" @("--oat-file=$odex", '--header-only', '--no-disassemble')
    Aot3SharedCheck "$Phase oatdump header contains the real nested dependency context and speed-profile filter" `
      ($header.Exit -eq 0 -and $header.Out -match 'compiler-filter\s*=\s*speed-profile' -and
       $header.Out -match 'class-loader-context\s*=\s*PCL\[\]\{PCL\[' -and
       $header.Out.Contains('gesher-shared73-entry.jar') -and $header.Out.Contains('gesher-shared73-base.jar')) $header.Out
    $code = Aot2Run "$Dir/oatdump.exe" @("--oat-file=$odex", "--dex-file=$sharedCode/base.apk", '--method-filter=hot', '--no-disassemble')
    Aot3SharedCheck "$Phase selected SharedActivity.hot has independently inspected compiled code" `
      ($code.Exit -eq 0 -and $code.Out.Contains('SharedActivity') -and $code.Out -match 'CODE:\s+\(code_offset=0x[0-9a-fA-F]+ size=[1-9][0-9]*\)') $code.Out
    $image = Aot2Run "$Dir/oatdump.exe" @("--app-image=$art", "--oat-file=$odex", "--dex-file=$sharedCode/base.apk",
      "--boot-image=$(NoDrive "$root/system")/framework/boot.art:$(NoDrive "$root/system")/framework/*",
      '--instruction-set=x86_64', '--runtime-arg', "-Xbootclasspath:$aotBcpApp", '--no-disassemble') @{} 120
    Aot3SharedCheck "$Phase oatdump reads the actual app image and SharedActivity" `
      ($image.Exit -eq 0 -and $image.Out -match 'IMAGE SIZE: [1-9]' -and $image.Out.Contains('SharedActivity')) $image.Out
  }

  function Aot3SharedRunModes([int] $Expected, [bool] $Aot) {
    foreach ($mode in 'single', 'device') {
      $from = Aot2Len $sharedPkgLog
      if ($mode -eq 'single') {
        $run = Aot2Run "$Dir/dalvikvm.exe" @("-Xbootclasspath:$aotBcpApp", $aotImage, '-Xusejit:true',
          'com.gesher.app.Main', "$sharedCode/base.apk") $sharedEnvironment 180
        $out = $run.Out
        $fresh = $true  # The directly owned VM exit is checked below.
      } else {
        # A package name starts the installed app without implicitly reinstalling it.
        $run = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'launch', $sharedPackage, '--headless') $sharedEnvironment 180
        $out = $run.Out + (Aot2Tail $sharedPkgLog $from)
        $fresh = $run.Out.Contains("gesher-app: the process of $sharedPackage ended (exit code 0)")
      }
      $methods = Aot2Methods $out
      $compiled = if ($Aot) {
        $methods -and $methods.Aot -gt 0 -and $methods.InterpretedAot -eq 0 -and $out -match 'app image loaded:.*base\.art:.*speed-profile'
      } else {
        $methods -and $methods.Aot -eq 0 -and $out -notmatch 'app image loaded:.*base\.art:'
      }
      $expectedText = "SharedActivity result=$Expected;"
      $passed = $run.Exit -eq 0 -and $fresh -and $out.Contains($expectedText) -and
        $out.Contains('order=library-before; distinct app/entry/dependency loaders=true') -and $compiled
      $kind = if ($Aot) { 'matching compiled code/image' } else { 'stale code/image rejected without reinstall' }
      Aot3SharedCheck "$mode result $Expected uses $kind and separate real library loaders" $passed $out
      foreach ($line in @($out -split "`n" | Where-Object { $_ -match 'SharedActivity result=|gesher: app (oat:|image loaded:|methods at exit:)|the process of com.gesher.sharedapp ended' })) {
        Write-Host "INFO  aot3 shared libraries ($mode): $line"
      }
    }
  }

  Aot3SharedArtifacts 'initial'
  Aot3SharedRunModes 1021000 $true
  $oldOat = (Get-FileHash "$sharedCode/oat/x86_64/base.odex").Hash
  $oldIdentity = [IO.File]::ReadAllText("$sharedCode/oat/x86_64/shared-libraries.txt")
  # Same registered dependency path, different actual DEX code. Do not install first.
  Copy-Item "$Dir/shared-base-v2.jar" $sharedBase -Force
  Aot3SharedRunModes 1037000 $false
  Aot3SharedCheck 'stale-artifact counterfactual leaves the original oat bytes and manifest untouched' `
    ((Get-FileHash "$sharedCode/oat/x86_64/base.odex").Hash -eq $oldOat -and
     [IO.File]::ReadAllText("$sharedCode/oat/x86_64/shared-libraries.txt") -eq $oldIdentity) $sharedCode

  $from = Aot2Len $sharedServerLog
  $refreshed = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'install', (NoDrive $sharedApk)) $sharedEnvironment 400
  $refreshLog = Aot2Tail $sharedServerLog $from
  Aot3SharedCheck 'an unchanged APK recompiles after the same-path dependency update and atomically publishes a new identity' `
    ($refreshed.Exit -eq 0 -and $refreshed.Out.Contains("is installed and unchanged ($sharedCode)") -and
     $refreshLog.Contains('shared-library context/content changed') -and
     (Get-FileHash "$sharedCode/oat/x86_64/base.odex").Hash -ne $oldOat -and
     [IO.File]::ReadAllText("$sharedCode/oat/x86_64/shared-libraries.txt") -ne $oldIdentity) ($refreshed.Out + $refreshLog)
  Aot3SharedArtifacts 'updated'
  Aot3SharedRunModes 1037000 $true

  $registry = Join-Path $root 'data/system/packages.list'
  $beforeRegistry = [IO.File]::ReadAllText($registry)
  $beforeApk = (Get-FileHash "$sharedCode/base.apk").Hash
  $bad = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'install', (NoDrive "$Dir/sharedlibrarybad.apk")) $sharedEnvironment 180
  Aot3SharedCheck 'a missing required library rejects version2 before changing the installed registry or version1 APK' `
    ($bad.Exit -eq 1 -and $bad.Out.Contains('INSTALL_FAILED_MISSING_SHARED_LIBRARY') -and
     [IO.File]::ReadAllText($registry) -eq $beforeRegistry -and (Get-FileHash "$sharedCode/base.apk").Hash -eq $beforeApk) $bad.Out
  Aot3SharedRunModes 1037000 $true
} catch {
  Aot3SharedCheck "fixture exception: $_" $false $_.ScriptStackTrace
} finally {
  try {
    $uninstall = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'uninstall', $sharedPackage) $sharedEnvironment 180
    $shutdown = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'shutdown') @{} 120
    if ($null -ne $sharedCode) {
      Aot3SharedCheck 'the installed shared-library app is uninstalled and its device shuts down' `
        ($uninstall.Exit -eq 0 -and $shutdown.Exit -eq 0 -and -not (Test-Path -LiteralPath $sharedCode)) ($uninstall.Out + $shutdown.Out)
    }
  } catch {
    Aot3SharedCheck "device cleanup exception: $_" $false $_.ScriptStackTrace
  } finally {
    try {
      if ($null -ne $sharedPropertiesBytes) {
        [IO.File]::WriteAllBytes($sharedBuildProp, $sharedPropertiesBytes)
        Aot3SharedCheck 'the temporary background-optimizer fixture policy is restored byte-for-byte' `
          ([Convert]::ToBase64String([IO.File]::ReadAllBytes($sharedBuildProp)) -eq [Convert]::ToBase64String($sharedPropertiesBytes)) $sharedBuildProp
      }
    } finally {
      if ($sharedFilesCreated) {
        foreach ($path in $sharedOwned) { Remove-Item -LiteralPath $path -Force -ErrorAction Continue }
        Aot3SharedCheck 'owned system-library XML/jars are removed after app uninstall and device shutdown' `
          (@($sharedOwned | Where-Object { Test-Path -LiteralPath $_ }).Count -eq 0) ($sharedOwned -join "`n")
      }
    }
  }
}
