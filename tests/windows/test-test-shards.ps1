$ErrorActionPreference = 'Stop'
$planner = Join-Path $PSScriptRoot 'helpers/Get-TestTrackPlan.ps1'
$manifestPath = Join-Path $PSScriptRoot 'test-shards.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$tracks = @(Get-ChildItem "$PSScriptRoot/tracks" -Filter *.ps1 | Sort-Object Name)
$asts = @{}
$functions = @{}
foreach ($track in $tracks) {
  $tokens = $null; $errors = $null
  $ast = [Management.Automation.Language.Parser]::ParseFile($track.FullName, [ref]$tokens, [ref]$errors)
  if ($errors) { throw "Track parse errors: $($track.Name): $errors" }
  $asts[$track.Name] = $ast
  foreach ($definition in $ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
  }, $false)) { $functions[$definition.Name] = $track.Name }
}
# Every manifest shard must run in CI and be accepted by the runner.
$shards = @($manifest.PSObject.Properties.Name)
$workflow = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../../.github/workflows/ci.yml') -Raw
$matrix = [regex]::Matches($workflow, '(?m)^\s+shard: \[([^\]]+)\]\s*$')
if ($matrix.Count -ne 1) { throw 'Expected one test-shards matrix in ci.yml' }
if (Compare-Object $shards @($matrix[0].Groups[1].Value -split ',\s*')) { throw "ci.yml shard matrix differs from test-shards.json: $($shards -join ', ')" }
$runnerSet = (Get-Command (Join-Path $PSScriptRoot 'run-tests.ps1')).Parameters['Shard'].Attributes |
  Where-Object { $_ -is [Management.Automation.ValidateSetAttribute] }
if (Compare-Object $shards @($runnerSet.ValidValues | Where-Object { $_ -ne 'all' })) { throw 'run-tests.ps1 Shard values differ from test-shards.json' }
$covered = @()
foreach ($shard in $shards) {
  $plan = & $planner -Shard $shard
  $listed = @(& (Join-Path $PSHOME 'pwsh') -NoProfile -File "$PSScriptRoot/run-tests.ps1" unused -Shard $shard -ListTracks)
  if ($LASTEXITCODE -ne 0 -or (Compare-Object $listed $plan.Tracks.Name)) { throw "Runner selection differs for $shard" }
  $covered += $plan.Tracks.Name
  $availableFunctions = @{}
  foreach ($definition in $plan.Helpers) {
    $ast = [scriptblock]::Create($definition).Ast
    $name = $ast.EndBlock.Statements[0].Name
    $availableFunctions[$name] = $true
    if ($definition -match '\$PSScriptRoot') { throw "Imported helper depends on its source directory: $name" }
  }
  foreach ($track in $plan.Tracks) {
    $ast = $asts[$track.Name]
    foreach ($include in [regex]::Matches($ast.Extent.Text, '\.\s*\(Join-Path \$PSScriptRoot ''([^'']+\.ps1)''\)')) {
      foreach ($name in @($functions.Keys)) {
        if ($functions[$name] -eq $include.Groups[1].Value) { $availableFunctions[$name] = $true }
      }
    }
    foreach ($definition in $ast.FindAll({ param($node)
      $node -is [Management.Automation.Language.FunctionDefinitionAst]
    }, $false)) { $availableFunctions[$definition.Name] = $true }
    foreach ($command in $ast.FindAll({ param($node)
      $node -is [Management.Automation.Language.CommandAst]
    }, $true)) {
      $name = $command.GetCommandName()
      if ($name -and $functions.ContainsKey($name) -and -not $availableFunctions.ContainsKey($name)) {
        $parent = $command.Parent
        while ($parent -and $parent -isnot [Management.Automation.Language.FunctionDefinitionAst]) { $parent = $parent.Parent }
        if ($parent -and $functions[$name] -in $plan.Tracks.Name) { continue }
        throw "Missing function dependency in $shard/$($track.Name): $name from $($functions[$name])"
      }
    }
  }
  # These tracks consume data or evidence produced by earlier tracks.
  foreach ($pair in @(
      @('30-graphics.ps1', '32-fonts.ps1'), @('34-windows.ps1', '49-locale.ps1'),
      @('41-native-activity.ps1', '47-arm64.ps1'), @('44-native2.ps1', '46-native3.ps1'),
      @('44-native2.ps1', '66-native3-wide.ps1'), @('44-native2.ps1', '69-arm64-fpcr.ps1'),
      @('44-native2.ps1', '75-arm64-fpsr.ps1'), @('44-native2.ps1', '82-net4-resolver.ps1'),
      @('44-native2.ps1', '85-arm64-environment.ps1'), @('44-native2.ps1', '89-net-udp-trunc.ps1'),
      @('44-native2.ps1', '92-net-dns-wire.ps1'), @('38-media.ps1', '59-media3.ps1'),
      @('50-services.ps1', '57-devsvc.ps1'), @('51-windows3.ps1', '54-windows4.ps1'),
      @('60-boot-image.ps1', '61-aot2.ps1'), @('61-aot2.ps1', '62-aot3.ps1'),
      @('61-aot2.ps1', '73-aot3-shared-libraries.ps1'), @('94-framework-fixes.ps1', '95-compatibility-followup.ps1'),
      @('94-framework-fixes.ps1', '96-deep-compatibility.ps1'), @('94-framework-fixes.ps1', '97-lifecycle-contracts.ps1'),
      @('94-framework-fixes.ps1', '98-system-contracts.ps1'), @('94-framework-fixes.ps1', '99-android-windows-contracts.ps1'))) {
    $names = @($plan.Tracks.Name)
    if ($pair[1] -in $names -and ($pair[0] -notin $names -or $names.IndexOf($pair[0]) -ge $names.IndexOf($pair[1]))) {
      throw "Missing or reordered state dependency in ${shard}: $($pair -join ' -> ')"
    }
  }
  Write-Host "PASS  $shard covers $($plan.Tracks.Count) tracks with original helpers and ordered prerequisites"
}
if (@($covered | Sort-Object -Unique).Count -ne $tracks.Count) { throw 'Incomplete track coverage' }

$tempBase = [IO.Path]::GetFullPath($(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }))
$temp = Join-Path $tempBase ('gesher-shards-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $temp | Out-Null
function Reject([string] $Name, [scriptblock] $Action) {
  $rejected = $false
  try { & $Action | Out-Null } catch { $rejected = $true }
  if (-not $rejected) { throw "Accepted invalid plan: $Name" }
  Write-Host "PASS  rejects $Name"
}
try {
  $fixtureManifest = Join-Path $temp 'shards.json'
  $fixtureTracks = Join-Path $temp 'tracks'
  New-Item -ItemType Directory $fixtureTracks | Out-Null
  foreach ($track in $tracks) { New-Item -ItemType File (Join-Path $fixtureTracks $track.Name) | Out-Null }
  Copy-Item $manifestPath $fixtureManifest
  New-Item -ItemType File (Join-Path $fixtureTracks '100-new.ps1') | Out-Null
  Reject 'unassigned new track' { & $planner -Shard native -TrackDirectory $fixtureTracks -ManifestPath $fixtureManifest }
  Remove-Item -LiteralPath (Join-Path $fixtureTracks '100-new.ps1')
  Remove-Item -LiteralPath (Join-Path $fixtureTracks $tracks[0].Name)
  Reject 'missing assigned track' { & $planner -Shard native -TrackDirectory $fixtureTracks -ManifestPath $fixtureManifest }
  $manifest.native.tracks += $manifest.desktop.tracks[0]
  $manifest | ConvertTo-Json -Depth 6 | Set-Content $fixtureManifest
  Reject 'duplicate owner' { & $planner -Shard native -ManifestPath $fixtureManifest }
  Reject 'unknown shard' { & $planner -Shard absent }
  Copy-Item $manifestPath $fixtureManifest -Force
  New-Item -ItemType File (Join-Path $fixtureTracks $tracks[0].Name) | Out-Null
  Reject 'missing helper definition' { & $planner -Shard native -TrackDirectory $fixtureTracks -ManifestPath $fixtureManifest }

  # Execute the production track loop and verdict with disposable tracks.
  $runner = [scriptblock]::Create((Get-Content "$PSScriptRoot/run-tests.ps1" -Raw)).Ast
  $loop = @($runner.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.ForEachStatementAst] -and $_.Condition.Extent.Text -eq '$testTracks' })
  if ($loop.Count -ne 1) { throw 'Production track loop not found' }
  $verdict = $runner.EndBlock.Statements[-1].Extent.Text
  $trackFile = Join-Path $temp 'fixture.ps1'
  $harness = Join-Path $temp 'run.ps1'
  @'
param([string] $TrackFile)
$ErrorActionPreference = 'Stop'
$failed = 0
$Shard = 'fixture'
$testTracks = @(Get-ChildItem -LiteralPath (Split-Path $TrackFile) -Filter '*-track.ps1' | Sort-Object Name)
'@ + "`n$($loop[0].Extent.Text)`n$verdict" | Set-Content $harness
  foreach ($case in @(
      @{ Name = 'success'; Body = 'Write-Host "PASS  fixture"'; Exit = 0 },
      @{ Name = 'assertion failure'; Body = '$script:failed++; Write-Host "FAIL  fixture"'; Exit = 1 },
      @{ Name = 'exception'; Body = 'throw "fixture error"'; Exit = 1 })) {
    $firstTrack = Join-Path $temp '01-track.ps1'
    $secondTrack = Join-Path $temp '02-track.ps1'
    # Existing tracks use $t for their own measurements/fixtures. Dot-sourcing
    # them must not erase the runner's timing or failure-report track identity.
    Set-Content $firstTrack ('function SharedFixture { "original helper" }; $fixtureState = 42; $t = @{}' + "`n" + $case.Body)
    Set-Content $secondTrack 'if ((SharedFixture) -ne "original helper" -or $fixtureState -ne 42) { throw "Lost track scope" }'
    $output = & (Join-Path $PSHOME 'pwsh') -NoProfile -File $harness $trackFile 2>&1
    if ($LASTEXITCODE -ne $case.Exit) { throw "Wrong verdict for $($case.Name): $LASTEXITCODE $output" }
    $text = $output -join "`n"
    if ($case.Name -eq 'exception') {
      if ($text -notmatch 'FAIL  test script 01-track\.ps1:') { throw "Lost failure track identity: $text" }
    } else {
      foreach ($name in '01-track.ps1', '02-track.ps1') {
        if ($text -notmatch ('TRACK_END shard=fixture track=' + [regex]::Escape($name) + ' elapsedMs=\d+')) {
          throw "Lost completed track identity: $text"
        }
      }
    }
    Write-Host "PASS  production loop propagates $($case.Name)"
  }
} finally {
  if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($temp)) -ne $tempBase.TrimEnd([IO.Path]::DirectorySeparatorChar)) {
    throw 'Refusing cleanup outside the test temporary directory'
  }
  Remove-Item -LiteralPath $temp -Recurse -Force
}
exit 0
