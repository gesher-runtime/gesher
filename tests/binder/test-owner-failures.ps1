param([Parameter(Mandatory)][string]$Dir)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/trusted-sg-client.ps1"
$broker = (Resolve-Path -LiteralPath (Join-Path $Dir 'binderd.exe')).Path
$actor = (Resolve-Path -LiteralPath (Join-Path $Dir 'binder_sg_actor.exe')).Path
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
