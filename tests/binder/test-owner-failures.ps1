param([Parameter(Mandatory)][string]$Dir)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/trusted-sg-client.ps1"
$broker = (Resolve-Path -LiteralPath (Join-Path $Dir 'binderd.exe')).Path
$actor = (Resolve-Path -LiteralPath (Join-Path $Dir 'binder_sg_actor.exe')).Path
if (-not [TrustedSgJob]::FixtureProveCloseFailure()) { throw 'checked native close failure was not retained and retried' }
Write-Host 'SG_CHECKED_CLOSE failure_retained=1 exact_retry_closed=1'
foreach ($stage in 1,2,3,4,5,6) {
  $failed = $false
  try {
    $python = (Get-Command python -CommandType Application | Select-Object -First 1).Source
    [void][TrustedSgJob]::Start($python,
        (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'trusted_sg_owner.py')).Path, $stage)
  } catch { $failed = $true }
  if (-not $failed -or -not [TrustedSgJob]::LastStartupRollbackConfirmed) {
    throw "partial owner setup $stage did not confirm exact process/job/pipe cleanup"
  }
  Write-Host "SG_OWNER_PARTIAL stage=$stage job_empty=1 owner_signaled=1 streams_closed=1"
}
$captureOwner = Start-TrustedSgOwner
try {
  $failed = $false
  try {
    [void](Invoke-TrustedSgOwner $captureOwner @{ op='begin'; broker=$broker; endpoint='sg-capture-fault';
        environment=@{ GESHER_BINDER='sg-capture-fault' }; capture_fault='after-start' })
  } catch { $failed = $true }
  if (-not $failed) { throw 'capture setup injection did not fail' }
} finally { try { Close-TrustedSgOwner $captureOwner } catch { } }
if (-not $captureOwner.Closed -or -not $captureOwner.Error.Result.Contains('SG_CAPTURE_FAULT after-start=1') -or
    -not $captureOwner.Error.Result.Contains('SG_CAPTURE_CLEANUP drains=1 joined=1')) {
  throw 'capture startup failure did not join its registered drain and retire owner'
}
Write-Host 'SG_CAPTURE_FAILURE after-start=1 drain_joined=1 job_empty=1 owner_signaled=1'
foreach ($target in 'begin','spawn') {
  foreach ($phase in 'before-publish-crash','before-publish-hang','after-resume-crash','after-resume-hang') {
    $caseDir = Join-Path $env:RUNNER_TEMP ('sg-fault-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $caseDir | Out-Null
    $device = 'sg-fault-' + [guid]::NewGuid().ToString('N')
    $environment = @{ GESHER_BINDER = $device }
    $owner = Start-TrustedSgOwner
    try {
      $begin = @{ op='begin'; broker=$broker; endpoint=$device; environment=$environment }
      if ($target -eq 'begin') { $begin.fixture_fault = $phase }
      $failed = $false
      try {
        [void](Invoke-TrustedSgOwner $owner $begin)
        if ($target -eq 'spawn') {
          [void](Invoke-TrustedSgOwner $owner @{ op='spawn'; args=@($actor, 'server', 'normal', (Join-Path $caseDir 'actor'));
              role='@servicemanager'; environment=$environment; fixture_fault=$phase })
        }
      } catch { $failed = $true }
      if (-not $failed) { throw 'injected owner fault did not fail the request' }
    } finally {
      try { Close-TrustedSgOwner $owner } catch { }
    }
    if (-not $owner.Closed) { throw 'owner job or exact process cleanup unconfirmed' }
    $log = $owner.Error.Result
    if (-not $log.Contains("SG_OWNER_FAULT phase=$phase pid=")) { throw 'actual injected phase marker missing' }
    $snapshot = @(Get-ChildItem -LiteralPath $caseDir -File | ForEach-Object { "$($_.Name):$($_.Length):$($_.LastWriteTimeUtc.Ticks)" }) -join '|'
    Start-Sleep -Milliseconds 500
    $after = @(Get-ChildItem -LiteralPath $caseDir -File | ForEach-Object { "$($_.Name):$($_.Length):$($_.LastWriteTimeUtc.Ticks)" }) -join '|'
    if ($snapshot -cne $after) { throw 'actor files changed after job retirement' }
    if ($phase.StartsWith('before-publish') -and (Test-Path -LiteralPath (Join-Path $caseDir 'actor.ready'))) {
      throw 'unpublished suspended actor ran'
    }
    Write-Host "SG_OWNER_FAILURE target=$target phase=$phase job_empty=1 owner_signaled=1 postcleanup_stable=1"
  }
}
