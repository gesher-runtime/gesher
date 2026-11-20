param(
  [Parameter(Mandatory)] [ValidateSet('original', 'rebuilt', 'trace')] [string] $Mode,
  [Parameter(Mandatory)] [string] $Out
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Out = (Resolve-Path -LiteralPath $Out).Path
$results = Join-Path $Out 'results'
$setup = @(Get-ChildItem -LiteralPath (Join-Path $Out 'installer') -Filter 'gesher-setup-*.exe')
if ($setup.Count -ne 1) { throw 'Exactly one verified original installer is required' }
$actualSetup = (Get-FileHash -LiteralPath $setup[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualSetup -cne '8d365400eb3d3802399341db5b83942360f764da1ac9ca0de9ad583cd329107a') {
  throw 'Original main installer changed before execution'
}
if (-not $env:GESHER_TEST_BUILD_TOOLS) { throw 'Real SDK APK identity collector is required' }
$env:GESHER_WEBVIEW_DEBUG = $null
$env:GESHER_TRACE_MISSING_BOOT_CLASSES = '1'
$phases = Join-Path $results 'native-phases'
New-Item -ItemType Directory -Path $phases | Out-Null
$env:GESHER_QA_NOTES_PHASE_DIR = $phases
$app = Join-Path $env:LOCALAPPDATA 'Programs/Gesher'
# Identical installer invocation, moved before the unchanged runner so the
# selected diagnostic bridge can be substituted explicitly and hashed.
$install = Start-Process $setup[0].FullName -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
if ($install.ExitCode -ne 0) { throw "Original installer failed: $($install.ExitCode)" }
$bridge = Join-Path $app 'libgesher_webview_bridge.so'
$originalBridge = (Get-FileHash -LiteralPath $bridge -Algorithm SHA256).Hash.ToLowerInvariant()
if ($originalBridge -cne '10ba637f8d360446e5d8908d986676b8bb0486f8d30b159731084b570532bc5d') {
  throw 'Original installed native bridge differs'
}
if ($Mode -ne 'original') {
  $receipt = Get-Content -LiteralPath (Join-Path $results 'bridge-receipt.json') -Raw | ConvertFrom-Json
  $name = "$Mode-libgesher_webview_bridge.so"
  $source = Join-Path (Join-Path $Out 'bridges') $name
  $digest = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($digest -cne $receipt.producer.outputs.$name) { throw 'Selected producer bridge changed' }
  Copy-Item -LiteralPath $source -Destination $bridge
  if ((Get-FileHash -LiteralPath $bridge -Algorithm SHA256).Hash.ToLowerInvariant() -cne $digest) {
    throw 'Installed diagnostic bridge differs from selected producer'
  }
}
$invocation = [guid]::NewGuid().ToString('N')
$env:GESHER_E2E_INVOCATION_ID = $invocation
$e2e = Join-Path $results 'e2e'
$started = [DateTime]::UtcNow.ToString('o')
$clock = [Diagnostics.Stopwatch]::StartNew()
$exitCode = 127
$exception = $null
try {
  # Every APK, original scenario, screenshot, wait, assertion and retirement
  # body stays exact. SkipInstall only retains the explicitly bound DLL above.
  $arguments = @('-NoProfile', '-File', (Join-Path $root 'tests/e2e/run-e2e.ps1'),
      '-Setup', $setup[0].FullName, '-SkipInstall', '-Out', $e2e, '-Only', 'org.billthefarmer.notes',
      '-InvocationId', $invocation, '-RequireApkIdentity', '-BuildTools', $env:GESHER_TEST_BUILD_TOOLS)
  & pwsh @arguments 2>&1 | Tee-Object -FilePath (Join-Path $results 'original-runner-output.log')
  $exitCode = $LASTEXITCODE
} catch {
  $exception = $_.Exception.Message
  throw
} finally {
  $clock.Stop()
  $installed = [ordered]@{}
  foreach ($name in @('dalvikvm.exe', 'gesher-app.jar', 'framework.jar', 'build.prop',
                      'libgesher_webview_bridge.so')) {
    $path = Join-Path $app $name
    $installed[$name] = if (Test-Path -LiteralPath $path -PathType Leaf) {
      (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    } else { $null }
  }
  [ordered]@{
    source = '28b2617e35df63a1cb0fb7c9678b5d3f41b90a9a'
    diagnostic_head = $env:GITHUB_SHA
    diagnostic_run = $env:GITHUB_RUN_ID
    diagnostic_attempt = $env:GITHUB_RUN_ATTEMPT
    mode = 'original'
    native_phase_mode = $Mode
    debug_enabled = $false
    invocation_id = $invocation
    started = $started
    completed = [DateTime]::UtcNow.ToString('o')
    elapsed_ms = $clock.ElapsedMilliseconds
    original_exit = $exitCode
    exception = $exception
    installer_sha256 = $actualSetup
    original_bridge_sha256 = $originalBridge
    installed_files = $installed
    scope = 'Exact original scenario using an explicitly bound historical/rebuilt/observation bridge; default debug disabled.'
  } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $results 'invocation.json') -Encoding utf8
}
exit $exitCode
