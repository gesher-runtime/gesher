# Additional compatibility contracts beyond PRs #10, #11 and #12.
# FixesRun and the actual ART boot arguments are provided by 94-framework-fixes.
$deepWifiOut = FixesRun ($dvmFixes + 'gesher.services.WifiDataContractTest') 90
$deepWifiCode = $LASTEXITCODE
Check 'framework: Wi-Fi data APIs and real native Parcelable round trips' {
  $global:LASTEXITCODE = $deepWifiCode
  $deepWifiOut
} @('WifiDataContractTest all passed:', 'parcel=true')

$deepNetworkOut = FixesRun ($dvmFixes + 'gesher.services.NetworkSnapshotContractTest') 90
$deepNetworkCode = $LASTEXITCODE
Check 'framework: network route, address, snapshot and proxy contracts with native Parcel' {
  $global:LASTEXITCODE = $deepNetworkCode
  $deepNetworkOut
} @('NetworkSnapshotContractTest all passed:', 'parcel=true')

$deepSurfaceOut = RunTimed "$Dir/media_surface_geometry_contract_test.exe" @() 90
$deepSurfaceCode = $LASTEXITCODE
Check 'graphics: actual QueueSurface and Win32Surface geometry and damage contracts' {
  $global:LASTEXITCODE = $deepSurfaceCode
  $deepSurfaceOut
} @('surface geometry contracts:', 'checks, 0 failures')

$deepAudioEnvironment = @{}
foreach ($name in 'ANDROID_DATA', 'GESHER_AUDIO') {
  $deepAudioEnvironment[$name] = @{
    Present = Test-Path "Env:$name"
    Value = [Environment]::GetEnvironmentVariable($name, 'Process')
  }
}
$deepAudioRoot = Join-Path $env:RUNNER_TEMP ('audio-state-contracts-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $deepAudioRoot | Out-Null
try {
  $env:ANDROID_DATA = NoDrive $deepAudioRoot
  $env:GESHER_AUDIO = 'null'
  $deepAudioOut = FixesRun ($dvmFixes + 'gesher.services.AudioStateContractTest') 90
  $deepAudioCode = $LASTEXITCODE
  Check 'media: actual AudioTrack JNI inactive writes, write interruption and static replay' {
    $global:LASTEXITCODE = $deepAudioCode
    $deepAudioOut
  } @('AudioStateContractTest all passed:')
} finally {
  foreach ($name in $deepAudioEnvironment.Keys) {
    $saved = $deepAudioEnvironment[$name]
    if ($saved.Present) { Set-Item "Env:$name" $saved.Value }
    else { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
  }
  Remove-Item -LiteralPath $deepAudioRoot -Recurse -Force -ErrorAction Stop
}
