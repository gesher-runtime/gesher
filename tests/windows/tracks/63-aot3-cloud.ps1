# Install-time cloud profiles: the real installer, pinned profman, dex2oat,
# oatdump and app process. 61/62 supply the bounded runner and inspection helpers.

function Aot3Metadata([string] $Path, [byte[]] $Profile) {
  $stream = [IO.File]::Open($Path, [IO.FileMode]::Create, [IO.FileAccess]::Write)
  $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
  try {
    if ($null -ne $Profile) {
      $entry = $zip.CreateEntry('primary.prof').Open()
      try { $entry.Write($Profile, 0, $Profile.Length) } finally { $entry.Dispose() }
    } else {
      $entry = $zip.CreateEntry('manifest.json').Open()
      try { $entry.WriteByte(123); $entry.WriteByte(125) } finally { $entry.Dispose() }
    }
  } finally { $zip.Dispose(); $stream.Dispose() }
}

try {
  $cloudRoot = Join-Path $aotRoot 'cloud'
  New-Item -ItemType Directory -Force $cloudRoot | Out-Null
  $cloudApk = Join-Path $cloudRoot 'lifecycle.apk'
  $cloudDm = Join-Path $cloudRoot 'lifecycle.dm'
  $cloudProfile = Join-Path $cloudRoot 'primary.prof'
  $cloudText = Join-Path $cloudRoot 'profile.txt'
  Copy-Item "$Dir/lifecycle.apk" $cloudApk
  [IO.File]::WriteAllText($cloudText, "Lcom/gesher/lifecycle/LifecycleActivity;`nHSLcom/gesher/lifecycle/LifecycleActivity;->onCreate(Landroid/os/Bundle;)V`n", [Text.UTF8Encoding]::new($false))
  $created = Aot2Run "$Dir/profman.exe" @("--create-profile-from=$(NoDrive $cloudText)",
    "--reference-profile-file=$(NoDrive $cloudProfile)", "--apk=$(NoDrive $cloudApk)",
    "--dex-location=$(NoDrive $cloudApk)")
  if ($created.Exit -ne 0 -or (Aot2Size $cloudProfile) -eq 0) {
    throw "cloud profile fixture creation failed: $($created.Out)"
  }
  $validProfile = [IO.File]::ReadAllBytes($cloudProfile)
  Aot3Metadata $cloudDm $validProfile
  # Use the production JIT/AOT path; forcing -Xint deliberately interprets AOT methods.
  $cloudEnv = @{ GESHER_LOG_EXECUTION = '1'; GESHER_APP_WINDOW = 'headless'; GESHER_APP_EXECUTION = 'jit' }
  $cloudPkg = 'com.gesher.lifecycle'
  $cloudServer = Join-Path $root 'logs/system_server.log'
  $cloudPkgLog = Join-Path $root "logs/$cloudPkg.log"
  $null = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'shutdown') @{} 120
  $null = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'uninstall', $cloudPkg) $cloudEnv 400

  function Aot3CloudInstall([string] $Filter, [string] $Import) {
    $from = Aot2Len $cloudServer
    $result = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'install', (NoDrive $cloudApk)) $cloudEnv 400
    $log = Aot2Tail $cloudServer $from
    if ($result.Exit -ne 0 -or $result.Out -notmatch '(installed|updated) com\.gesher\.lifecycle \(version \d+\) in (\S+)') {
      throw "cloud install failed: $($result.Out)`n$log"
    }
    $path = $Matches[2]
    $header = Aot2Run "$Dir/oatdump.exe" @("--oat-file=$path/oat/x86_64/base.odex", '--header-only', '--no-disassemble')
    if ($header.Exit -ne 0 -or $header.Out -notmatch "compiler-filter\s*=\s*$Filter" -or $log -notmatch $Import) {
      throw "cloud install filter/import mismatch: $($result.Out)`n$log`n$($header.Out)"
    }
    [pscustomobject]@{ Path = $path; Out = $result.Out; Log = $log; Header = $header.Out }
  }

  $accepted = Aot3CloudInstall 'speed-profile' 'cloud profile accepted from'
  $installedDm = "$($accepted.Path)/base.dm"
  $sameBytes = (Get-FileHash $installedDm).Hash -eq (Get-FileHash $cloudDm).Hash
  $committed = Join-Path $root "data/misc/profiles/ref/$cloudPkg/primary.prof"
  Aot3Check 'install consumes matching base.dm, preserves its bytes and compiles speed-profile immediately' ($sameBytes -and (Aot2Size $committed) -gt 0) ($accepted.Out + $accepted.Log)
  $compiled = Aot2Run "$Dir/oatdump.exe" @("--oat-file=$($accepted.Path)/oat/x86_64/base.odex",
    "--dex-file=$($accepted.Path)/base.apk", '--method-filter=onCreate', '--no-disassemble')
  Aot3Check 'cloud-selected LifecycleActivity.onCreate has independently inspected compiled code' ($compiled.Exit -eq 0 -and $compiled.Out -match 'LifecycleActivity' -and $compiled.Out -match 'onCreate' -and $compiled.Out -match 'CODE:\s+\(code_offset=0x[0-9a-fA-F]+ size=[1-9][0-9]*\)') $compiled.Out
  $from = Aot2Len $cloudPkgLog
  $launch = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'launch', (NoDrive $cloudApk), '--headless') $cloudEnv 400
  $app = Aot2Tail $cloudPkgLog $from
  $methods = Aot2Methods $app
  Aot3Check 'first cloud-profile launch loads its app image and executes compiled app methods' ($launch.Exit -eq 0 -and $launch.Out.Contains('gesher-app: done') -and $app.Contains('LifecycleActivity onCreate') -and $app -match 'app image loaded:.*base\.art:.*speed-profile' -and $methods -and $methods.Aot -gt 0 -and $methods.InterpretedAot -eq 0) ($launch.Out + $app)

  # A bad archive must fail before it stops/replaces the installed package.
  $registry = Join-Path $root 'data/system/packages.list'
  $beforeRegistry = [IO.File]::ReadAllText($registry)
  $beforeDm = (Get-FileHash $installedDm).Hash
  [IO.File]::WriteAllText($cloudDm, 'invalid metadata archive')
  $badZip = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'install', (NoDrive $cloudApk)) $cloudEnv 400
  $preserved = $badZip.Exit -ne 0 -and $badZip.Out.Contains('INSTALL_FAILED_BAD_DEX_METADATA') -and
    [IO.File]::ReadAllText($registry) -eq $beforeRegistry -and (Get-FileHash $installedDm).Hash -eq $beforeDm
  Aot3Check 'malformed dex metadata rejects the update without changing the installed package or metadata' $preserved $badZip.Out
  $oldLaunch = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'launch', "$($accepted.Path)/base.apk", '--headless') $cloudEnv 400
  Aot3Check 'the previously installed app remains runnable after metadata rejection' ($oldLaunch.Exit -eq 0 -and $oldLaunch.Out.Contains('gesher-app: done')) $oldLaunch.Out

  $null = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'uninstall', $cloudPkg) $cloudEnv 400
  Aot3Metadata $cloudDm ([Text.Encoding]::ASCII.GetBytes('invalid profile bytes'))
  $invalid = Aot3CloudInstall 'verify' 'cloud profile rejected \(profman 24\)'
  Aot3Check 'valid metadata ZIP with corrupt profile falls back to verify and leaves no staged profile' (-not (Test-Path "$committed.cloud") -and -not (Test-Path "$($invalid.Path)/oat/x86_64/base.art")) $invalid.Log
  $fallbackLaunch = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'launch', (NoDrive $cloudApk), '--headless') $cloudEnv 400
  Aot3Check 'profile rejection preserves successful installation and APK execution' ($fallbackLaunch.Exit -eq 0 -and $fallbackLaunch.Out.Contains('gesher-app: done')) $fallbackLaunch.Out

  # The APK is byte-identical: a changed companion must still be processed.
  Aot3Metadata $cloudDm $validProfile
  $replacement = Aot3CloudInstall 'speed-profile' 'cloud profile accepted from'
  Aot3Check 'an unchanged APK with new metadata is reprocessed instead of returning unchanged' ($replacement.Path -ne $invalid.Path -and (Get-FileHash "$($replacement.Path)/base.dm").Hash -eq (Get-FileHash $cloudDm).Hash) $replacement.Log

  $other = Join-Path $cloudRoot 'other.prof'
  $generated = Aot2Run "$Dir/profman.exe" @("--generate-test-profile=$(NoDrive $other)",
    '--generate-test-profile-method-percentage=100', '--generate-test-profile-class-percentage=100',
    '--generate-test-profile-seed=42', "--apk=$(NoDrive "$Dir/popupapp.apk")",
    "--dex-location=$(NoDrive "$Dir/popupapp.apk")")
  if ($generated.Exit -ne 0 -or (Aot2Size $other) -eq 0) { throw "mismatch profile creation failed: $($generated.Out)" }
  $null = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'uninstall', $cloudPkg) $cloudEnv 400
  Aot3Metadata $cloudDm ([IO.File]::ReadAllBytes($other))
  $mismatch = Aot3CloudInstall 'verify' 'cloud profile rejected \(profman 21\)'
  Aot3Check 'profman checksum/type/method mismatch falls back to verify without committing a profile' ((Aot2Size $committed) -eq 0 -and -not (Test-Path "$committed.cloud")) $mismatch.Log
  $null = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'uninstall', $cloudPkg) $cloudEnv 400
  Aot3Metadata $cloudDm $null
  $missing = Aot3CloudInstall 'verify' 'cloud profile rejected \(profman 21\)'
  Aot3Check 'a valid metadata ZIP without primary.prof follows the pinned empty-profile fallback' ((Aot2Size $committed) -eq 0) $missing.Log
  # Simulate the temporary file left by an interrupted import before uninstall.
  [IO.File]::WriteAllBytes("$committed.cloud", $validProfile)
  $null = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'uninstall', $cloudPkg) $cloudEnv 400
  Aot3Check 'uninstall removes the installed metadata and cloud/current/reference profiles' (-not (Test-Path "$($missing.Path)/base.dm") -and -not (Test-Path $committed) -and -not (Test-Path "$committed.cloud") -and -not (Test-Path (Join-Path $root "data/misc/profiles/cur/0/$cloudPkg/primary.prof"))) ''
} catch {
  Aot3Check "cloud-profile checks threw: $($_.Exception.Message)" $false $_.ScriptStackTrace
} finally {
  $null = Aot2Run "$Dir/dalvikvm.exe" ($aotLauncher + 'shutdown') @{} 120
}
