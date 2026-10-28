# CI-only artifact provenance: selected qualified full and this exact component run.
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true') { throw 'CI only' }
$baseId = '40596167427'
$baseSha = '30db7709838839d28be68d8b467a57b26a3ac667'
function Test-RunIdentity($run, [string] $id, [string] $sha, [int] $attempt, [string] $repository) {
  return "$($run.id)" -ceq $id -and $run.head_sha -ceq $sha -and $run.run_attempt -eq $attempt -and
    "$($run.repository.id)" -ceq $repository -and "$($run.head_repository.id)" -ceq $repository
}
# Exercise the same identity gate on synthetic mismatches, before using real metadata.
$sample = [pscustomobject]@{ id='7'; head_sha='source'; run_attempt=2; repository=@{id='8'}; head_repository=@{id='8'} }
if (-not (Test-RunIdentity $sample '7' 'source' 2 '8') -or
    (Test-RunIdentity $sample '7' 'wrong-source' 2 '8') -or
    (Test-RunIdentity $sample '7' 'source' 3 '8') -or
    (Test-RunIdentity $sample '9' 'source' 2 '8') -or
    (Test-RunIdentity $sample '7' 'source' 2 '9')) { throw 'Run identity negative controls failed' }
Write-Host 'SURFACE_IDENTITY_NEGATIVES wrong-source/attempt/run/repository=rejected'
if ((git rev-parse HEAD) -cne $env:GITHUB_SHA -or $LASTEXITCODE -ne 0) { throw 'Checkout mismatch' }
$allowed = @('.github/workflows/ci.yml','.github/workflows/media4-surface-build.yml',
  '.github/workflows/media4-surface-component.yml','cmake/aosp/media.cmake',
  'compat/media/surface/frame_invalidation.h','compat/media/surface/surface_jni.cc',
  'tests/apps/surfacegl/AndroidManifest.xml','tests/apps/surfacegl/build.sh',
  'tests/apps/surfacegl/java/com/gesher/surfacegl/GlActivity.java',
  'tests/apps/surfacegl/java/com/gesher/surfacegl/InvalidationActivity.java',
  'tests/apps/surfacegl/jni/surfacegl.c','tests/e2e/apps.json','tests/e2e/run-e2e.ps1',
  'tests/surfaces/invalidation_test.cc','tests/windows/run-surface-invalidation-component.ps1',
  'tests/windows/tracks/80-surface-invalidation.ps1','tools/surface/verify-component-suppliers.ps1','tools/surface/download-verified-artifact.py','tools/surface/test-verified-artifact.py')
$changed = @(git diff --name-only $baseSha HEAD)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect source delta' }
foreach ($path in $changed) { if (-not $path.StartsWith('docs/') -and $path -cnotin $allowed) { throw "Unreviewed scaffold source delta: $path" } }
$base = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$baseId" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or -not (Test-RunIdentity $base $baseId $baseSha 1 $env:GITHUB_REPOSITORY_ID) -or $base.name -cne 'ci' -or $base.conclusion -cne 'success') { throw 'Qualified full identity mismatch' }
$current = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or -not (Test-RunIdentity $current $env:GITHUB_RUN_ID $env:GITHUB_SHA $env:GITHUB_RUN_ATTEMPT $env:GITHUB_REPOSITORY_ID)) { throw 'Component run identity mismatch' }
$currentJobs = @{}
foreach ($entry in @(@($baseId,1,$baseSha,@('build','framework / framework','graphics / build','angle / build','media / media','webview / build','exec / exec','boot-image / compile')), @($env:GITHUB_RUN_ID,$env:GITHUB_RUN_ATTEMPT,$env:GITHUB_SHA,@('framework / framework','surfaces / apps','ndk / native2','translator / build','host')))) {
  $jobs = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$($entry[0])/attempts/$($entry[1])/jobs?per_page=100" | ConvertFrom-Json
  if ($LASTEXITCODE -ne 0 -or $jobs.total_count -gt 100) { throw 'Cannot inspect producer jobs' }
  foreach ($name in $entry[3]) {
    $rows = @($jobs.jobs | Where-Object name -CEQ $name)
    if ($rows.Count -ne 1 -or $rows[0].conclusion -cne 'success' -or $rows[0].head_sha -cne $entry[2] -or "$($rows[0].run_id)" -cne "$($entry[0])" -or "$($rows[0].run_attempt)" -cne "$($entry[1])") { throw "Producer mismatch $name" }
    if ("$($entry[0])" -ceq $env:GITHUB_RUN_ID) { $currentJobs[$name] = $rows[0] }
  }
}
$baseline = @{
 'windows-x64'=@('12779280757','sha256:d0a990f7d6163d043597c60c03360160c1081e1ead450783a0c71e67100a242d');
 'framework'=@('12776002815','sha256:64482ac13d29f0c8a488483f30ad02d9bb1021a5b3dcf53a99e9a44850dd1f09');
 'graphics'=@('12777265454','sha256:51d7e33fd603cc90347eceafe2cb220f538c49c80f7c7bc0f5a86dd59d32bec7');
 'angle'=@('12775634982','sha256:87f328b6c5461f5dfb2c93a5297addbde7e9719e012b878cb980690a1795f209');
 'media'=@('12777416551','sha256:38f7916541491d6d0a8d1097e34f922058669cc1577c8772cbe7cd075c23c5a1');
 'webview'=@('12775764440','sha256:a69a1e4e30f9409b6cc1dda17ee707ed3b6334725a9aad8fe541c7a06488fe28');
 'exec'=@('12777180459','sha256:320b1dee79542cd0273b05ea092e0d9c8607ab116e7b6c4cbd41a5a6dc22e7a6');
 'boot-image'=@('12777738989','sha256:c0eb14751722b28dac9d0024e168cd8fb8e6441798f8b747063d092432baee9d')
}
foreach ($source in @(@($baseId,$baseSha,'base',$baseline.Keys),@($env:GITHUB_RUN_ID,$env:GITHUB_SHA,'current',@('framework','surfaces','native2','arm64','media4-surface-component-host')))) {
  $inventory = gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$($source[0])/artifacts?per_page=100" | ConvertFrom-Json
  if ($LASTEXITCODE -ne 0 -or $inventory.total_count -gt 100) { throw 'Artifact inventory unavailable' }
  foreach ($name in $source[3]) {
    $rows = @($inventory.artifacts | Where-Object name -CEQ $name)
    if ($rows.Count -ne 1) { throw "Artifact count mismatch $name" }
    $a = $rows[0]
    if ($a.expired -or $a.size_in_bytes -le 0 -or $a.digest -cnotmatch '^sha256:[0-9a-f]{64}$' -or "$($a.workflow_run.id)" -cne "$($source[0])" -or $a.workflow_run.head_sha -cne $source[1] -or "$($a.workflow_run.repository_id)" -cne $env:GITHUB_REPOSITORY_ID -or "$($a.workflow_run.head_repository_id)" -cne $env:GITHUB_REPOSITORY_ID) { throw "Artifact provenance mismatch $name" }
    if ($source[2] -eq 'base' -and ("$($a.id)" -cne $baseline[$name][0] -or $a.digest -cne $baseline[$name][1])) { throw "Qualified artifact identity changed $name" }
    if ($source[2] -eq 'current') {
      $producer = @{ framework='framework / framework'; surfaces='surfaces / apps'; native2='ndk / native2'; arm64='translator / build'; 'media4-surface-component-host'='host' }[$name]
      $job = $currentJobs[$producer]
      if ($null -eq $job -or [datetime]$a.created_at -lt [datetime]$job.started_at -or [datetime]$a.created_at -gt [datetime]$job.completed_at) { throw "Artifact not produced in current attempt: $name" }
    }
    $key = "$($source[2])_$($name.Replace('-','_'))"
    "$key=$($a.id)" >> $env:GITHUB_OUTPUT
    "${key}_digest=$($a.digest)" >> $env:GITHUB_OUTPUT
    Write-Host "SURFACE_SUPPLIER role=$($source[2]) name=$name id=$($a.id) digest=$($a.digest) source=$($source[1]) run=$($source[0])"
  }
}
