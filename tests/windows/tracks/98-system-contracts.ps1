# Android system contracts, in both the full and framework-only Windows suites.
# Track94 supplies FixesRun and the real ART boot arguments in $dvmFixes.

# Cookie callbacks use a private profile and real WebView2; no network navigation.
. "$PSScriptRoot/../webview/Runtime.ps1"
$cookieContractProfile = Join-Path ([IO.Path]::GetTempPath()) ('gesher-cookie-contract-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $cookieContractProfile -ErrorAction Stop | Out-Null
try {
  $cookieContractOut = FixesRun ($dvmFixes + 'gesher.services.CookieCallbackContractTest' + $cookieContractProfile) 120
  $cookieContractCode = $LASTEXITCODE
  Check 'webview: cookie callbacks return to each calling HandlerThread' {
    $global:LASTEXITCODE = $cookieContractCode
    $cookieContractOut
  } @('CookieCallbackContractTest all passed:', 'real HandlerThreads and native WebView2 callbacks')
} finally {
  # WebView2 can retire its child process shortly after the VM exits. Remove only
  # this fixture's profile, with a finite retry; never kill unrelated browsers.
  $cookieCleanupDeadline = [DateTime]::UtcNow.AddSeconds(10)
  while (Test-Path -LiteralPath $cookieContractProfile) {
    try { Remove-Item -LiteralPath $cookieContractProfile -Recurse -Force -ErrorAction Stop }
    catch {
      if ([DateTime]::UtcNow -ge $cookieCleanupDeadline) { throw }
      Start-Sleep -Milliseconds 200
    }
  }
}
