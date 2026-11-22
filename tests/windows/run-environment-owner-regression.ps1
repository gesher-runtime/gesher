# Ordinary current-build host regression. The enclosing suite verifies the
# current source/run/attempt runtime artifact; this does not reuse or qualify
# the separately pinned font component's supplier/owner evidence.
param(
  [Parameter(Mandatory)][string]$Fixture,
  [ValidateSet('strict', 'functional')][string]$Scope = 'strict'
)
$ErrorActionPreference = 'Stop'
$fixturePath = (Resolve-Path -LiteralPath $Fixture).Path
$beforeHash = (Get-FileHash -LiteralPath $fixturePath -Algorithm SHA256).Hash.ToLowerInvariant()
$source = (& git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $source -cne $env:GITHUB_SHA -or $env:GITHUB_ACTIONS -cne 'true') {
  throw 'Environment regression requires the current CI checkout'
}
$tree = (& git rev-parse 'HEAD^{tree}').Trim()
if ($LASTEXITCODE -ne 0) { throw 'Environment regression tree identity unavailable' }
Write-Host ('ENVIRONMENT_HOST_REGRESSION_INPUT ' + (@{
  source = $source; tree = $tree; repository = $env:GITHUB_REPOSITORY
  run = $env:GITHUB_RUN_ID; attempt = $env:GITHUB_RUN_ATTEMPT
  executable_sha256 = $beforeHash; role = 'ORDINARY_HOST_FONT_REGRESSION'
  scope = $Scope; held_writer_callback_requested = ($Scope -ceq 'strict')
  historical_component_qualification = $false
} | ConvertTo-Json -Compress))
$work = Join-Path $env:RUNNER_TEMP ('environment-regression-' + [guid]::NewGuid())
$null = New-Item -ItemType Directory -Path $work
$passed = 0
$failedModes = [Collections.Generic.List[string]]::new()
try {
  foreach ($mode in @('wide', 'missing', 'empty', 'invalid-wide', 'supplementary')) {
    $info = [Diagnostics.ProcessStartInfo]::new($fixturePath)
    $info.ArgumentList.Add($mode)
    if ($Scope -ceq 'functional') { $info.ArgumentList.Add('--functional') }
    $info.WorkingDirectory = $work
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($key in @('ANDROID_ROOT', 'GESHER_WINDOWS_FONTS')) {
      if ($mode -ceq 'missing') { $null = $info.Environment.Remove($key) }
      else {
        $info.Environment[$key] = switch -CaseSensitive ($mode) {
          'wide' { 'initial-אב space' }
          'empty' { '' }
          'invalid-wide' { [string][char]0xd800 }
          'supplementary' { 'initial-' + [char]::ConvertFromUtf32(0x1f600) + ' space' }
        }
      }
    }
    $child = $null
    try {
      $child = [Diagnostics.Process]::Start($info)
      $stdout = $child.StandardOutput.ReadToEndAsync()
      $stderr = $child.StandardError.ReadToEndAsync()
      $finished = $child.WaitForExit(90000)
      if (-not $finished) {
        try { $child.Kill($true) } catch { }
        $null = $child.WaitForExit(5000)
      }
      $pipes = $stdout.Wait(10000) -and $stderr.Wait(10000)
      if (-not $finished -or -not $pipes -or -not $stdout.IsCompletedSuccessfully -or -not $stderr.IsCompletedSuccessfully) {
        if ($stdout.IsCompletedSuccessfully) { Write-Output $stdout.Result }
        if ($stderr.IsCompletedSuccessfully) { Write-Output $stderr.Result }
        throw 'Bounded child/pipe completion failed'
      }
      $text = $stdout.Result + "`n" + $stderr.Result
      $passPrefix = if ($Scope -ceq 'strict') { 'FONT_ENV_HOST_PASS' } else { 'FONT_ENV_FUNCTIONAL_PASS' }
      $pass = [regex]::Matches($text, '(?m)^' + $passPrefix + ' mode=' + [regex]::Escape($mode) + ' [^\r\n]*')
      $wide = [regex]::Matches($text, '(?m)^FONT_ENV_HOST_INITIAL_WIDE key=(ANDROID_ROOT|GESHER_WINDOWS_FONTS) [^\r\n]*')
      $borrowed = [regex]::Matches($text, '(?m)^FONT_ENV_HOST_BORROWED unrelated_native_visible=1 mutable_alias=1 empty_posix=1 renamed_identity=1 wide_transition=1 last_writer=1\r?$')
      $wct = 'FONT_ENV_WCT_PASS own_process=1 specific_writer_wait=CRITICAL_SECTION held_owner=INVALID_HANDLER concurrent_read_callback=1 unset_writer=1 true_kernel_completion=1 privilege_restored=1 session_token_handles_closed=1 arbitrary_writer_reentry_qualified=0'
      $scopeEvidence = if ($Scope -ceq 'strict') {
        $text.Contains($wct) -and $text.Contains('FONT_ENV_WCT_PRIVILEGE_RESTORE success=1 error=0') -and
          -not $text.Contains('FONT_ENV_FUNCTIONAL_PASS')
      } else {
        $pass.Count -eq 1 -and $pass[0].Value.Contains('pre_mutation_reentrant_read=1') -and
          $pass[0].Value.Contains('concurrency=1000 handles_closed=1 held_writer_callback_qualified=0 crt_owner_qualified=0') -and
          -not $text.Contains('FONT_ENV_WCT_PASS') -and -not $text.Contains('FONT_ENV_HOST_PASS')
      }
      if ($child.ExitCode -ne 0 -or $pass.Count -ne 1 -or $wide.Count -ne 2 -or $borrowed.Count -ne 1 -or
          -not $scopeEvidence -or
          $text.Contains('FONT_ENV_HOST_FAIL')) {
        Write-Output $text
        throw "Native font/environment assertions failed with exit $($child.ExitCode)"
      }
      # Preserve actual received UTF16, native outcomes and WCT proof in the
      # run log; never print the inherited environment or unrelated variables.
      foreach ($line in ($text -split '\r?\n')) {
        if ($line -cmatch '^FONT_ENV_HOST_(INITIAL_WIDE|BORROWED|PASS) ' -or
            $line -cmatch '^FONT_ENV_FUNCTIONAL_PASS ' -or $line -cmatch '^FONT_ENV_WCT_(PASS|PRIVILEGE_RESTORE) ') {
          Write-Host $line
        }
      }
      $passed++
    } catch {
      $failedModes.Add($mode)
      Write-Output "Environment owner regression failed: mode=$mode; $($_.Exception.Message)"
    } finally {
      if ($child) {
        if (-not $child.HasExited) {
          try { $child.Kill($true) } catch { }
          $null = $child.WaitForExit(5000)
        }
        $child.Dispose()
      }
    }
  }
} finally {
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ((Get-FileHash -LiteralPath $fixturePath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $beforeHash) {
  $failedModes.Add('binary-changed')
}
if ($passed -eq 5 -and $failedModes.Count -eq 0) {
  $global:LASTEXITCODE = 0
  if ($Scope -ceq 'strict') { Write-Output 'font environment regression: 5 cases passed' }
  else { Write-Output 'font environment functional regression: 5 cases passed; held_writer_callback_qualified=0 crt_owner_qualified=0' }
} else {
  $global:LASTEXITCODE = 1
  Write-Output ('font environment regression: scope=' + $Scope + ' failed modes=' + ($failedModes -join ','))
}
