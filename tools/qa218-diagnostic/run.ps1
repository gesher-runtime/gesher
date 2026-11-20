$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true' -or -not $env:GESHER_QA218_RESULTS) { throw 'CI diagnostic context absent' }
$root = [IO.Path]::GetFullPath($env:GESHER_QA218_RESULTS)
$prefix = [IO.Path]::GetFullPath($env:RUNNER_TEMP).TrimEnd('\') + '\'
if (-not $root.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or -not [IO.File]::Exists((Join-Path $root 'source-manifest.json'))) { throw 'Owned verified result root absent' }
$phases = Join-Path $root ('phases-' + [Guid]::NewGuid().ToString('N'))
$null = [IO.Directory]::CreateDirectory($phases)
$env:GESHER_QA218_PHASE_DIR = $phases
$started = [DateTime]::UtcNow
$outcome = 'not-completed'
$failure = ''
try {
  # The complete original producer/controller/fixture/provider runner is unchanged.
  & (Join-Path $PSScriptRoot '../../tests/windows/helpers/run-geocoder-winrt-component.ps1')
  $outcome = 'passed-original-component'
} catch {
  $outcome = 'failed-original-component'
  $failure = $_.Exception.Message
  throw
} finally {
  $env:GESHER_QA218_PHASE_DIR = $null
  $files = @([IO.Directory]::GetFiles($phases, '*.txt') | Sort-Object)
  if ($files.Count -gt 128) { throw 'Unbounded diagnostic phase output' }
  $rows = @()
  $incomplete = @()
  foreach ($path in $files) {
    if ([IO.FileInfo]::new($path).Length -gt 2048) { throw 'Unbounded diagnostic phase record' }
    $line = [IO.File]::ReadAllText($path)
    $parts = $line.Split('|')
    if ($parts.Count -ne 6 -or $line -notmatch '^[0-9]+\|[0-9]+\|[0-9]+\|[^|]+\|[a-zA-Z0-9_.-]+\|[a-f0-9]*$') {
      # An owned child can be retired during its tiny observation write.
      # Preserve that partial evidence and the original failure/timeout.
      $incomplete += @{ File = [IO.Path]::GetFileName($path); Bytes = [IO.FileInfo]::new($path).Length; Text = $line }
      continue
    }
    $rows += @{ Pid = [int]$parts[0]; Tick = [long]$parts[1]; Frequency = [long]$parts[2]; Utc = $parts[3]; Phase = $parts[4]; Detail = $parts[5] }
  }
  foreach ($row in @($rows | Sort-Object Tick)) { Write-Host ('QA218_PHASE ' + ($row | ConvertTo-Json -Compress)) }
  foreach ($row in @($rows | Where-Object { $_.Phase -eq 'assembly-load-begin' })) {
    if ($row.Detail -cne 'deec6ef8ebf0c6b8e8e405d1b65690ee290a54941290c06765bc960382544a14') { throw 'Observed runtime DLL differs from the qualified unchanged compiled source' }
  }
  if ($outcome -eq 'passed-original-component' -and @($rows | Where-Object { $_.Phase -eq 'fixture-entry' }).Count -ne 1) { throw 'Successful fixture lacks entry observation' }
  $summary = @{ Source = $env:GITHUB_SHA; Base = 'fea065241a5c2589d839c94fc1de579c2d02c838'; Repository = $env:GITHUB_REPOSITORY; RunId = $env:GITHUB_RUN_ID; RunAttempt = $env:GITHUB_RUN_ATTEMPT; StartedUtc = $started.ToString('o'); FinishedUtc = [DateTime]::UtcNow.ToString('o'); Outcome = $outcome; Failure = $failure; RuntimeDeadlineMs = 45000; CompilerCancellationMs = 40000; PhaseCount = $rows.Count; Phases = $rows; IncompleteRecords = $incomplete }
  [IO.File]::WriteAllText((Join-Path $root 'observation.json'), ($summary | ConvertTo-Json -Depth 8))
  if ($outcome -eq 'passed-original-component' -and $incomplete.Count -ne 0) { throw 'Successful component has incomplete phase evidence' }
}
