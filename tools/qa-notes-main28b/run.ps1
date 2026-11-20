param(
  [Parameter(Mandatory)] [ValidateSet('original', 'debug')] [string] $Mode,
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
if ($Mode -eq 'debug') { $env:GESHER_WEBVIEW_DEBUG = '1' }
else { $env:GESHER_WEBVIEW_DEBUG = $null }
# This is also set in the original main e2e invocation.
$env:GESHER_TRACE_MISSING_BOOT_CLASSES = '1'
$invocation = [guid]::NewGuid().ToString('N')
$env:GESHER_E2E_INVOCATION_ID = $invocation
$e2e = Join-Path $results 'e2e'
$started = [DateTime]::UtcNow.ToString('o')
$clock = [Diagnostics.Stopwatch]::StartNew()
$exitCode = 127
$exception = $null
try {
  # No manifest, APK, production binary, input, assertion, timing or retirement
  # code is changed. Each mode runs in its own fresh hosted VM/profile.
  $arguments = @('-NoProfile', '-File', (Join-Path $root 'tests/e2e/run-e2e.ps1'),
      '-Setup', $setup[0].FullName, '-Out', $e2e, '-Only', 'org.billthefarmer.notes',
      '-InvocationId', $invocation, '-RequireApkIdentity', '-BuildTools', $env:GESHER_TEST_BUILD_TOOLS)
  & pwsh @arguments 2>&1 | Tee-Object -FilePath (Join-Path $results 'original-runner-output.log')
  $exitCode = $LASTEXITCODE
} catch {
  $exception = $_.Exception.Message
  throw
} finally {
  $clock.Stop()
  $app = Join-Path $env:LOCALAPPDATA 'Programs/Gesher'
  $installed = [ordered]@{}
  foreach ($name in @('dalvikvm.exe', 'gesher-app.jar', 'framework.jar', 'build.prop',
                      'libgesher_webview_bridge.so')) {
    $path = Join-Path $app $name
    $installed[$name] = if (Test-Path -LiteralPath $path -PathType Leaf) {
      (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    } else { $null }
  }
  $receipt = [ordered]@{
    source = '28b2617e35df63a1cb0fb7c9678b5d3f41b90a9a'
    diagnostic_head = $env:GITHUB_SHA
    diagnostic_run = $env:GITHUB_RUN_ID
    diagnostic_attempt = $env:GITHUB_RUN_ATTEMPT
    mode = $Mode
    debug_enabled = ($Mode -eq 'debug')
    invocation_id = $invocation
    started = $started
    completed = [DateTime]::UtcNow.ToString('o')
    elapsed_ms = $clock.ElapsedMilliseconds
    original_exit = $exitCode
    exception = $exception
    installer_sha256 = $actualSetup
    installed_files = $installed
    scope = 'Original Notes-only scenario; debug mode changes only existing GESHER_WEBVIEW_DEBUG. No phase/result is fabricated.'
  }
  $receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $results 'invocation.json') -Encoding utf8
}
exit $exitCode
