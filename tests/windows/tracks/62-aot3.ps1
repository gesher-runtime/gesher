# Independently inspect 61-aot2's real oat/vdex/app image with ART's oatdump.
# Dot-sourced after 61: Aot2Run/NoDrive, $aotRoot and $aotBcpApp are available.

function Aot3Check([string] $Name, [bool] $Ok, [string] $Output) {
  if ($Ok) {
    Write-Host "PASS  aot3: $Name"
  } else {
    Write-Host "FAIL  aot3: $Name"
    $script:failed++
    ($Output -split "`n" | Select-Object -First 100) | Out-Host
  }
}

try {
  $oatdump = Join-Path $Dir 'oatdump.exe'
  if (-not (Test-Path $oatdump)) { throw 'oatdump.exe is missing' }
  $appOat = Join-Path $aotRoot 'single/oat/x86_64/lifecycle.odex'
  $appArt = $appOat -replace '\.odex$', '.art'
  $appDex = Join-Path $aotRoot 'single/lifecycle.apk'
  $bootArt = "$(NoDrive "$root/system")/framework/boot.art"
  $frameworkOat = Join-Path $root 'system/framework/x86_64/boot-framework.oat'

  $header = Aot2Run $oatdump @("--oat-file=$(NoDrive $frameworkOat)", '--header-only', '--no-disassemble')
  Aot3Check 'oatdump reads the shipped framework oat header and speed-profile filter' ($header.Exit -eq 0 -and $header.Out -match 'speed-profile' -and $header.Out -match 'DEX FILE COUNT:') $header.Out

  $app = Aot2Run $oatdump @("--oat-file=$(NoDrive $appOat)", "--dex-file=$(NoDrive $appDex)", '--no-disassemble')
  $compiled = $app.Exit -eq 0 -and $app.Out -match 'speed-profile' -and
    $app.Out -match 'LifecycleActivity' -and $app.Out -match '\((Some|All)Compiled\)' -and
    $app.Out -match 'CODE:\s+\(code_offset=0x[0-9a-fA-F]+ size=[1-9][0-9]*\)'
  Aot3Check 'oatdump independently lists lifecycle.apk classes and compiled oat code' $compiled $app.Out
  ($app.Out -split "`n" | Where-Object { $_ -match 'compiler-filter|LifecycleActivity|\((Some|All)Compiled\)|CODE:' } | Select-Object -First 25) | Out-Host

  $image = Aot2Run $oatdump @("--app-image=$(NoDrive $appArt)", "--oat-file=$(NoDrive $appOat)",
    "--dex-file=$(NoDrive $appDex)", "--boot-image=${bootArt}:$(NoDrive "$root/system")/framework/*",
    '--instruction-set=x86_64', '--runtime-arg', "-Xbootclasspath:$aotBcpApp", '--no-disassemble') @{} 120
  Aot3Check 'oatdump loads the app image with its boot extensions and finds LifecycleActivity' ($image.Exit -eq 0 -and $image.Out -match 'IMAGE LOCATION:.*lifecycle\.art' -and $image.Out -match 'IMAGE SIZE: [1-9]' -and $image.Out -match 'LifecycleActivity') $image.Out

  $missing = Aot2Run $oatdump @("--oat-file=$(NoDrive $aotRoot)/missing.odex", '--header-only')
  Aot3Check 'oatdump rejects a missing oat file without a crash' ($missing.Exit -eq 1 -and $missing.Out -match '(?i)failed|error|no such|cannot') $missing.Out
} catch {
  Aot3Check "oatdump checks threw: $($_.Exception.Message)" $false $_.ScriptStackTrace
}
